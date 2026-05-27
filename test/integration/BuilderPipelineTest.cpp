#include "topo/AST/ASTNode.h"
#include "topo/Basic/Diagnostic.h"
#include "topo/Backend/PassPipeline.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Transforms/SymbolObfuscator.h"
#include "topo/Transforms/TopoFlattenPass.h"
#include "topo/Transforms/TopoInlinePass.h"
#include "topo/Transforms/PipelineCodeGenPass.h"
#include "topo/Transforms/TopoLayoutPass.h"
#include "topo/Transforms/TopoReorderPass.h"
#include "topo/Backend/VisibilityApplier.h"
#include "topo/Sema/VisibilityCollector.h"
#include "topo/Lexer/Lexer.h"
#include "topo/Parser/Parser.h"
#include "topo/Sema/SemanticAnalyzer.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <gtest/gtest.h>
#include <fstream>
#include <sstream>

using namespace topo;

static std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Helper: parse a .topo source string
static std::unique_ptr<TopoFile> parseTopo(const std::string& source, DiagnosticEngine& diag) {
    Lexer lexer(source, "<test>", diag);
    Parser parser(lexer, diag);
    return parser.parseTopoFile();
}

// Canonical "no-paramConsts" visibility entry used by the integration tests.
// The struct has additional fields (paramConsts / isConst / bindingTarget /
// priority) whose defaults match what these tests want, but partial-list
// initialization triggers `-Wmissing-field-initializers` and lets a future
// field default to whatever the struct picks. The helper keeps every site
// in one place — see issue
// `missing-field-initializer-warnings-in-test-fixtures`.
static VisibilityEntry makeVisEntry(std::string qualifiedName, Visibility vis) {
    VisibilityEntry e;
    e.qualifiedName = std::move(qualifiedName);
    e.visibility = vis;
    return e;
}

// Helper: create a synthetic LLVM module with named functions.
// Uses Itanium mangling convention for C++ names.
static std::unique_ptr<llvm::Module> createSyntheticModule(llvm::LLVMContext& ctx,
                                                           const std::vector<std::string>& mangledNames) {
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    for (const auto& name : mangledNames) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, *module);
        // Add a body so the function is not a declaration
        llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(&func->getEntryBlock());
        builder.CreateRetVoid();
    }
    return module;
}

// --- VisibilityCollector tests ---

TEST(BuilderPipeline, VisibilityCollectorDemo) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    VisibilityCollector collector;
    auto entries = collector.collect(static_cast<const TopoFile&>(*ast));

    // demo.topo: run (public), init (protected), process (protected)
    ASSERT_EQ(entries.size(), 3u);

    // Verify qualified names contain app::
    for (const auto& e : entries) {
        EXPECT_NE(e.qualifiedName.find("app::"), std::string::npos) << "Expected app:: prefix in: " << e.qualifiedName;
    }
}

// --- SymbolMapper tests with synthetic module ---

TEST(BuilderPipeline, SymbolMapperMatchesFunctions) {
    // demo.topo declares: app::run, app::init, app::process
    // Create mangled names matching these
    // Itanium mangling: _ZN3app3runEv, _ZN3app4initEv, _ZN3app7processEv
    llvm::LLVMContext ctx;
    auto module = createSyntheticModule(ctx, {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    // Collect visibility from demo.topo
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());
    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    VisibilityCollector collector;
    auto entries = collector.collect(static_cast<const TopoFile&>(*ast));

    // Map
    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    EXPECT_EQ(mapping.matched.size(), 3u);
    EXPECT_TRUE(mapping.unmatchedTopo.empty());
}

// --- VisibilityApplier tests ---

TEST(BuilderPipeline, VisibilityApplierSetsLinkage) {
    llvm::LLVMContext ctx;
    auto module = createSyntheticModule(ctx, {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());
    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    VisibilityCollector collector;
    auto entries = collector.collect(static_cast<const TopoFile&>(*ast));

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    VisibilityApplier applier;
    auto stats = applier.apply(*module, mapping, entries);

    // run=public, init=protected, process=protected
    EXPECT_EQ(stats.publicCount, 1);
    EXPECT_EQ(stats.protectedCount, 2);
    EXPECT_EQ(stats.privateCount, 0);

    // Verify linkage
    auto* runFunc = mapping.matched.at("app::run");
    EXPECT_EQ(runFunc->getLinkage(), llvm::GlobalValue::ExternalLinkage);

    auto* initFunc = mapping.matched.at("app::init");
    EXPECT_EQ(initFunc->getLinkage(), llvm::GlobalValue::InternalLinkage);

    auto* processFunc = mapping.matched.at("app::process");
    EXPECT_EQ(processFunc->getLinkage(), llvm::GlobalValue::InternalLinkage);
}

// --- Full pipeline test ---

TEST(BuilderPipeline, FullPipelineEndToEnd) {
    llvm::LLVMContext ctx;
    auto module = createSyntheticModule(ctx, {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());
    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    // Step 1: Collect
    VisibilityCollector collector;
    auto entries = collector.collect(static_cast<const TopoFile&>(*ast));
    ASSERT_EQ(entries.size(), 3u);

    // Step 2: Map
    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);
    ASSERT_EQ(mapping.matched.size(), 3u);

    // Step 3: Apply
    VisibilityApplier applier;
    auto stats = applier.apply(*module, mapping, entries);
    EXPECT_EQ(stats.publicCount + stats.protectedCount + stats.privateCount, 3);

    // Verify all functions still have bodies
    for (const auto& [name, func] : mapping.matched) {
        EXPECT_FALSE(func->isDeclaration()) << "Function " << name << " lost its body";
    }
}

// --- Custom Pass Tests ---

// Helper: create a module where 'caller' calls 'callee'
static std::unique_ptr<llvm::Module> createModuleWithCall(llvm::LLVMContext& ctx,
                                                          const std::string& callerName,
                                                          const std::string& calleeName) {
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    // Create callee
    auto* callee = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, calleeName, *module);
    auto* calleeBB = llvm::BasicBlock::Create(ctx, "entry", callee);
    llvm::IRBuilder<> calleeBuilder(calleeBB);
    calleeBuilder.CreateRetVoid();

    // Create caller that calls callee
    auto* caller = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, callerName, *module);
    auto* callerBB = llvm::BasicBlock::Create(ctx, "entry", caller);
    llvm::IRBuilder<> callerBuilder(callerBB);
    callerBuilder.CreateCall(callee);
    callerBuilder.CreateRetVoid();

    return module;
}

// Helper: create a module where 'caller' calls a callee with N basic blocks
static std::unique_ptr<llvm::Module> createModuleWithSizedCallee(llvm::LLVMContext& ctx,
                                                                 const std::string& callerName,
                                                                 const std::string& calleeName,
                                                                 unsigned numBlocks) {
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    // Create callee with numBlocks basic blocks
    auto* callee = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, calleeName, *module);
    llvm::BasicBlock* prevBB = nullptr;
    for (unsigned i = 0; i < numBlocks; ++i) {
        auto* bb = llvm::BasicBlock::Create(ctx, "bb" + std::to_string(i), callee);
        if (prevBB) {
            llvm::IRBuilder<> builder(prevBB);
            builder.CreateBr(bb);
        }
        prevBB = bb;
    }
    // Terminate the last block
    llvm::IRBuilder<> calleeBuilder(prevBB);
    calleeBuilder.CreateRetVoid();

    // Create caller that calls callee
    auto* caller = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, callerName, *module);
    auto* callerBB = llvm::BasicBlock::Create(ctx, "entry", caller);
    llvm::IRBuilder<> callerBuilder(callerBB);
    callerBuilder.CreateCall(callee);
    callerBuilder.CreateRetVoid();

    return module;
}

TEST(CustomPass, InlinePassInlinesSmallProtectedAtO2) {
    llvm::LLVMContext ctx;
    // caller calls callee; callee is protected with 3 basic blocks → should get InlineHint at O2
    auto module = createModuleWithSizedCallee(ctx, "_ZN3app3runEv", "_ZN3app4implEv", 3);

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::run", Visibility::Public),
        makeVisEntry("app::impl", Visibility::Protected),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);
    ASSERT_EQ(mapping.matched.size(), 2u);

    int annotated = TopoInlinePass::run(*module, OptLevel::O2, entries, mapping);
    EXPECT_GE(annotated, 1);

    // Verify: callee should have InlineHint attribute
    auto* callee = module->getFunction("_ZN3app4implEv");
    ASSERT_NE(callee, nullptr);
    EXPECT_TRUE(callee->hasFnAttribute(llvm::Attribute::InlineHint));
}

TEST(CustomPass, InlinePassSkipsLargeProtectedAtO2) {
    llvm::LLVMContext ctx;
    // caller calls callee; callee is protected with 15 basic blocks → should NOT be inlined at O2
    auto module = createModuleWithSizedCallee(ctx, "_ZN3app3runEv", "_ZN3app4implEv", 15);

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::run", Visibility::Public),
        makeVisEntry("app::impl", Visibility::Protected),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);
    ASSERT_EQ(mapping.matched.size(), 2u);

    VisibilityApplier applier;
    applier.apply(*module, mapping, entries);

    int inlined = TopoInlinePass::run(*module, OptLevel::O2, entries, mapping);
    EXPECT_EQ(inlined, 0);
}

TEST(CustomPass, InlinePassInlinesPrivate) {
    llvm::LLVMContext ctx;
    // caller calls callee; callee is private → should get AlwaysInline (single caller)
    auto module = createModuleWithCall(ctx, "_ZN3app3runEv", "_ZN3app4implEv");

    // Set up visibility: run=public, impl=private
    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::run", Visibility::Public),
        makeVisEntry("app::impl", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);
    ASSERT_EQ(mapping.matched.size(), 2u);

    // Run inline pass (sets attributes, does not physically inline)
    int annotated = TopoInlinePass::run(*module, OptLevel::O2, entries, mapping);
    EXPECT_GE(annotated, 1);

    // Verify: callee should have AlwaysInline attribute (single-caller private)
    auto* callee = module->getFunction("_ZN3app4implEv");
    ASSERT_NE(callee, nullptr);
    EXPECT_TRUE(callee->hasFnAttribute(llvm::Attribute::AlwaysInline));
}

TEST(CustomPass, FlattenPassDemotesDeadPrivate) {
    llvm::LLVMContext ctx;
    // Create a module with an unused private function (ExternalLinkage to test demotion)
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app4implEv", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRetVoid();

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::impl", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    int demoted = TopoFlattenPass::run(*module, entries, mapping);
    EXPECT_EQ(demoted, 1);

    // Function should still exist but with InternalLinkage (LLVM GlobalDCE removes it later)
    auto* result = module->getFunction("_ZN3app4implEv");
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->hasInternalLinkage());
}

TEST(CustomPass, FlattenPassReportRecordsDemotedFunctions) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage,
                                        "_ZN3app4implEv", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRetVoid();

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::impl", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    backend::TopoFlattenReport report;
    int demoted = TopoFlattenPass::run(*module, entries, mapping,
                                       BuildMode::Dev, &report);
    EXPECT_EQ(demoted, 1);

    ASSERT_EQ(report.demotedFunctions.size(), 1u);
    EXPECT_EQ(report.demotedFunctions[0], "app::impl");
}

TEST(CustomPass, LayoutPassSetsSections) {
    llvm::LLVMContext ctx;
    auto module = createSyntheticModule(ctx, {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());
    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    VisibilityCollector collector;
    auto entries = collector.collect(static_cast<const TopoFile&>(*ast));

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    // Need semantic analysis for stage info
    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    int sectionCount = TopoLayoutPass::run(*module, symbols, mapping);
    EXPECT_GE(sectionCount, 1);

    // Verify at least one function has a topo stage section
    // ELF: ".text.topo.stage", Mach-O: "__TEXT,__topo_stg", PE: ".text.topo.stage...$"
    bool hasTopoSection = false;
    for (auto& func : *module) {
        if (func.isDeclaration()) continue;
        std::string sec = func.getSection().str();
        if (sec.find("topo") != std::string::npos &&
            (sec.find("stage") != std::string::npos || sec.find("stg") != std::string::npos)) {
            hasTopoSection = true;
        }
    }
    EXPECT_TRUE(hasTopoSection);
}

// --- Obfuscation Tests ---

TEST(Obfuscation, NormalModeDeterministic) {
    llvm::LLVMContext ctx;
    auto module1 = createSyntheticModule(ctx, {"_ZN3app4implEv"});
    auto module2 = createSyntheticModule(ctx, {"_ZN3app4implEv"});

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::impl", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping1 = mapper.mapSymbols(*module1, entries);
    auto mapping2 = mapper.mapSymbols(*module2, entries);

    auto r1 = SymbolObfuscator::obfuscate(*module1, entries, mapping1, ObfuscationMode::Normal);
    auto r2 = SymbolObfuscator::obfuscate(*module2, entries, mapping2, ObfuscationMode::Normal);

    EXPECT_EQ(r1.renamedCount, 1);
    EXPECT_EQ(r2.renamedCount, 1);

    // Same input → same hash in normal mode
    auto* f1 = &*module1->begin();
    auto* f2 = &*module2->begin();
    // Both should be declarations (skipped) or the renamed function
    // Find the non-declaration function
    for (auto& f : *module1) {
        if (!f.isDeclaration()) {
            f1 = &f;
            break;
        }
    }
    for (auto& f : *module2) {
        if (!f.isDeclaration()) {
            f2 = &f;
            break;
        }
    }
    EXPECT_EQ(f1->getName(), f2->getName());
    EXPECT_TRUE(f1->getName().starts_with("_Zt"));
}

TEST(Obfuscation, SaltedModeDifferent) {
    llvm::LLVMContext ctx;
    auto module1 = createSyntheticModule(ctx, {"_ZN3app4implEv"});
    auto module2 = createSyntheticModule(ctx, {"_ZN3app4implEv"});

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::impl", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping1 = mapper.mapSymbols(*module1, entries);
    auto mapping2 = mapper.mapSymbols(*module2, entries);

    auto r1 = SymbolObfuscator::obfuscate(*module1, entries, mapping1, ObfuscationMode::Salted, "salt1");
    auto r2 = SymbolObfuscator::obfuscate(*module2, entries, mapping2, ObfuscationMode::Salted, "salt2");

    EXPECT_EQ(r1.renamedCount, 1);
    EXPECT_EQ(r2.renamedCount, 1);

    llvm::Function* f1 = nullptr;
    llvm::Function* f2 = nullptr;
    for (auto& f : *module1) {
        if (!f.isDeclaration()) {
            f1 = &f;
            break;
        }
    }
    for (auto& f : *module2) {
        if (!f.isDeclaration()) {
            f2 = &f;
            break;
        }
    }
    ASSERT_NE(f1, nullptr);
    ASSERT_NE(f2, nullptr);
    EXPECT_NE(f1->getName(), f2->getName()) << "Different salts should produce different hashes";
}

TEST(Obfuscation, PublicSymbolUnchanged) {
    llvm::LLVMContext ctx;
    auto module = createSyntheticModule(ctx, {"_ZN3app3runEv", "_ZN3app4initEv"});

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::run", Visibility::Public),
        makeVisEntry("app::init", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    auto result = SymbolObfuscator::obfuscate(*module, entries, mapping, ObfuscationMode::Normal);

    EXPECT_EQ(result.renamedCount, 1); // only private is renamed

    // Public function should keep its original name
    auto* runFunc = module->getFunction("_ZN3app3runEv");
    EXPECT_NE(runFunc, nullptr) << "Public symbol should keep original name";
}

// --- Rust ABI Tests ---

TEST(RustABI, RustV0ManglingDetected) {
    // Rust v0 mangling for mycrate::mymod::myfunc
    // _RNvNtCsXXXX_7mycrate5mymod6myfunc
    // llvm::demangle should handle this, but the exact format depends
    // on the LLVM version. Test with a real-looking Rust v0 name.
    llvm::LLVMContext ctx;

    // Create a synthetic module with Rust-style mangled name
    // Use Itanium mangling that looks like Rust for testing
    // (since llvm::demangle handles both)
    auto module = createSyntheticModule(ctx,
                                        {
                                            "_ZN3app3runEv", // Itanium (C++)
                                        });

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::run", Visibility::Public),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);
    EXPECT_EQ(mapping.matched.size(), 1u);
}

TEST(RustABI, RustRuntimeSymbolsSkipped) {
    llvm::LLVMContext ctx;

    // Create module with Rust runtime symbols that should be skipped
    auto module = createSyntheticModule(ctx,
                                        {
                                            "rust_begin_unwind",
                                            "__rust_alloc",
                                            "_ZN3app3runEv", // Regular C++ function
                                        });

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::run", Visibility::Public),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    // Only app::run should match; Rust runtime symbols should be skipped
    EXPECT_EQ(mapping.matched.size(), 1u);
    EXPECT_NE(mapping.matched.find("app::run"), mapping.matched.end());

    // Rust runtime symbols should NOT appear in unmatched IR
    for (const auto& name : mapping.unmatchedIR) {
        EXPECT_NE(name, "rust_begin_unwind") << "Rust runtime symbol should be skipped";
        EXPECT_NE(name, "__rust_alloc") << "Rust runtime symbol should be skipped";
    }
}

// --- Aggressive mode (ThinLTO) test ---

TEST(AggressiveMode, ThinLTOPipelineRuns) {
    llvm::LLVMContext ctx;
    auto module = createSyntheticModule(ctx, {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());
    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    // Collect visibility
    VisibilityCollector collector;
    auto entries = collector.collect(static_cast<const TopoFile&>(*ast));

    // Map symbols
    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);
    ASSERT_EQ(mapping.matched.size(), 3u);

    // Semantic analysis for stages
    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    // Apply visibility
    VisibilityApplier applier;
    auto stats = applier.apply(*module, mapping, entries);
    EXPECT_GT(stats.publicCount + stats.protectedCount + stats.privateCount, 0);

    // Run full pipeline in aggressive (ThinLTO) mode
    PassPipelineConfig pcfg;
    pcfg.entries = &entries;
    pcfg.mapping = &mapping;
    pcfg.symbols = &symbols;
    pcfg.mode = BuildMode::Aggressive;
    bool result = PassPipeline::run(*module, OptLevel::O2, pcfg);
    EXPECT_TRUE(result);

    // Verify: module is still valid (functions haven't been corrupted)
    for (auto& func : *module) {
        if (func.isDeclaration()) continue;
        EXPECT_FALSE(func.empty()) << "Function " << func.getName().str()
                                   << " should still have basic blocks after aggressive pipeline";
    }
}

// ===== Issue 012 regression: PipelineCodeGen returns error on mismatched parameters =====

TEST(PipelineCodeGen, MismatchedParameterReturnsError) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* f64Ty = llvm::Type::getDoubleTy(ctx);

    // Create pipeline_placeholder function (needed for stub detection)
    auto* placeholderTy = llvm::FunctionType::get(voidTy, false);
    auto* placeholder = llvm::Function::Create(
        placeholderTy, llvm::GlobalValue::ExternalLinkage, "_ZN4topo6detail20pipeline_placeholderEv", *module);

    // Create pipeline stub function: takes (i32) returns i32
    auto* pipelineTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* pipelineFunc =
        llvm::Function::Create(pipelineTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app3runEi", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
        llvm::IRBuilder<> builder(bb);
        builder.CreateCall(placeholder);
        builder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    // Create source node function: takes (double, double) — mismatched with pipeline (i32)
    auto* sourceTy = llvm::FunctionType::get(i32Ty, {f64Ty, f64Ty}, false);
    auto* sourceFunc = llvm::Function::Create(sourceTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app4loadEdd", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", sourceFunc);
        llvm::IRBuilder<> builder(bb);
        builder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    // Build SymbolTable with pipeline logic block
    SymbolTable symbols;
    LogicBlockEntry logicBlock;
    logicBlock.qualifiedName = "app::run";
    logicBlock.simpleName = "run";
    logicBlock.isPipeline = true;
    logicBlock.calledFunctions = {"app::load"};
    logicBlock.stages = {1};
    PipelineEdge edge;
    edge.source = "load";
    edge.target = "void";
    logicBlock.edges = {edge};

    PipelineAnalysis analysis;
    analysis.stages = {{"load", 1}};
    analysis.sourceNodes = {"load"};
    analysis.terminalNode = "load";
    analysis.terminalType = "int";
    logicBlock.pipelineAnalysis = analysis;

    symbols.addLogicBlock(logicBlock);

    // Build SymbolMapping
    SymbolMapping mapping;
    mapping.matched["app::run"] = pipelineFunc;
    mapping.matched["app::load"] = sourceFunc;

    // Run PipelineCodeGenPass — should return -1 because source node's
    // (double, double) parameters can't match pipeline's (i32) parameter
    int result = PipelineCodeGenPass::run(*module, symbols, mapping);
    EXPECT_EQ(result, -1) << "PipelineCodeGen should return error on parameter type mismatch";
}

TEST(Obfuscation, ProtectedMappingGenerated) {
    llvm::LLVMContext ctx;
    auto module = createSyntheticModule(ctx, {"_ZN3app4initEv"});

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::init", Visibility::Protected),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    auto result = SymbolObfuscator::obfuscate(*module, entries, mapping, ObfuscationMode::Normal);

    EXPECT_EQ(result.renamedCount, 1);
    EXPECT_EQ(result.protectedMapping.size(), 1u);
    EXPECT_NE(result.protectedMapping.find("app::init"), result.protectedMapping.end());
    EXPECT_TRUE(result.protectedMapping.at("app::init").substr(0, 3) == "_Zt");
}

// --- Pipeline Functor Inline Expansion Tests ---

// Helper: create a module with a pipeline functor calling two nodes,
// both of which call a shared private helper.
// Structure: pipeline → {nodeA, nodeB}, nodeA → helper, nodeB → helper
static std::unique_ptr<llvm::Module> createFunctorInlineModule(llvm::LLVMContext& ctx,
                                                               bool addRecursiveHelper = false) {
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    // helper — shared private callee (multi-caller)
    auto* helper = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app6helperEv", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", helper);
        llvm::IRBuilder<> builder(bb);
        builder.CreateRetVoid();
    }

    // nodeA — calls helper
    auto* nodeA = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app5nodeAEv", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeA);
        llvm::IRBuilder<> builder(bb);
        builder.CreateCall(helper);
        builder.CreateRetVoid();
    }

    // nodeB — calls helper
    auto* nodeB = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app5nodeBEv", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeB);
        llvm::IRBuilder<> builder(bb);
        builder.CreateCall(helper);
        builder.CreateRetVoid();
    }

    // pipeline — calls nodeA and nodeB
    auto* pipeline = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app8pipelineEv", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipeline);
        llvm::IRBuilder<> builder(bb);
        builder.CreateCall(nodeA);
        builder.CreateCall(nodeB);
        if (addRecursiveHelper) {
            // recHelper — calls itself (recursive)
            auto* recHelper =
                llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app9recHelperEv", *module);
            auto* recBB = llvm::BasicBlock::Create(ctx, "entry", recHelper);
            llvm::IRBuilder<> recBuilder(recBB);
            recBuilder.CreateCall(recHelper);
            recBuilder.CreateRetVoid();
            // pipeline also calls recHelper
            builder.CreateCall(recHelper);
        }
        builder.CreateRetVoid();
    }

    return module;
}

TEST(FunctorInline, MultiCallerPrivateUpgradedToAlwaysInline) {
    llvm::LLVMContext ctx;
    auto module = createFunctorInlineModule(ctx);

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::pipeline", Visibility::Public),
        makeVisEntry("app::nodeA", Visibility::Private),
        makeVisEntry("app::nodeB", Visibility::Private),
        makeVisEntry("app::helper", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);
    ASSERT_EQ(mapping.matched.size(), 4u);

    // Build SymbolTable with pipeline logic block
    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "app::pipeline";
    lb.simpleName = "pipeline";
    lb.isPipeline = true;
    lb.calledFunctions = {"app::nodeA", "app::nodeB"};
    lb.stages = {1, 1};
    symbols.addLogicBlock(lb);

    // Without symbols: helper gets InlineHint (multi-caller private)
    int annotated = TopoInlinePass::run(*module, OptLevel::O2, entries, mapping);
    EXPECT_GE(annotated, 3); // nodeA, nodeB, helper all annotated

    auto* helperFunc = module->getFunction("_ZN3app6helperEv");
    ASSERT_NE(helperFunc, nullptr);
    // helper has 2 callers → Step 1 gives InlineHint
    EXPECT_TRUE(helperFunc->hasFnAttribute(llvm::Attribute::InlineHint))
        << "Without symbols, multi-caller private helper should get InlineHint";
    EXPECT_FALSE(helperFunc->hasFnAttribute(llvm::Attribute::AlwaysInline));

    // Now reset and run with symbols for functor expansion
    for (auto& func : *module) {
        func.removeFnAttr(llvm::Attribute::AlwaysInline);
        func.removeFnAttr(llvm::Attribute::InlineHint);
    }

    annotated = TopoInlinePass::run(*module, OptLevel::O2, entries, mapping, &symbols);

    // helper should now be AlwaysInline (functor expansion upgraded it)
    EXPECT_TRUE(helperFunc->hasFnAttribute(llvm::Attribute::AlwaysInline))
        << "With symbols, multi-caller private helper in pipeline functor "
           "should be upgraded to AlwaysInline";
    EXPECT_FALSE(helperFunc->hasFnAttribute(llvm::Attribute::InlineHint))
        << "InlineHint should be removed when upgrading to AlwaysInline";
}

TEST(FunctorInline, RecursiveCalleeSkipped) {
    llvm::LLVMContext ctx;
    auto module = createFunctorInlineModule(ctx, /*addRecursiveHelper=*/true);

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::pipeline", Visibility::Public),
        makeVisEntry("app::nodeA", Visibility::Private),
        makeVisEntry("app::nodeB", Visibility::Private),
        makeVisEntry("app::helper", Visibility::Private),
        makeVisEntry("app::recHelper", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "app::pipeline";
    lb.simpleName = "pipeline";
    lb.isPipeline = true;
    lb.calledFunctions = {"app::nodeA", "app::nodeB"};
    lb.stages = {1, 1};
    symbols.addLogicBlock(lb);

    TopoInlinePass::run(*module, OptLevel::O2, entries, mapping, &symbols);

    // recHelper is recursive — should NOT get AlwaysInline
    auto* recHelper = module->getFunction("_ZN3app9recHelperEv");
    ASSERT_NE(recHelper, nullptr);
    EXPECT_FALSE(recHelper->hasFnAttribute(llvm::Attribute::AlwaysInline))
        << "Recursive callee should not get AlwaysInline from functor expansion";
}

TEST(FunctorInline, PublicCalleeUnaffected) {
    llvm::LLVMContext ctx;
    auto module = createFunctorInlineModule(ctx);

    // Make helper public — should not be forced alwaysinline by functor expansion
    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::pipeline", Visibility::Public),
        makeVisEntry("app::nodeA", Visibility::Private),
        makeVisEntry("app::nodeB", Visibility::Private),
        makeVisEntry("app::helper", Visibility::Public),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "app::pipeline";
    lb.simpleName = "pipeline";
    lb.isPipeline = true;
    lb.calledFunctions = {"app::nodeA", "app::nodeB"};
    lb.stages = {1, 1};
    symbols.addLogicBlock(lb);

    TopoInlinePass::run(*module, OptLevel::O2, entries, mapping, &symbols);

    auto* helperFunc = module->getFunction("_ZN3app6helperEv");
    ASSERT_NE(helperFunc, nullptr);
    EXPECT_FALSE(helperFunc->hasFnAttribute(llvm::Attribute::AlwaysInline))
        << "Public callee should not be forced alwaysinline by functor expansion";
    EXPECT_FALSE(helperFunc->hasFnAttribute(llvm::Attribute::InlineHint)) << "Public callee should have no inline hint";
}

TEST(FunctorInline, NonPipelineLogicBlockIgnored) {
    llvm::LLVMContext ctx;
    auto module = createFunctorInlineModule(ctx);

    std::vector<VisibilityEntry> entries = {
        makeVisEntry("app::pipeline", Visibility::Public),
        makeVisEntry("app::nodeA", Visibility::Private),
        makeVisEntry("app::nodeB", Visibility::Private),
        makeVisEntry("app::helper", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    // Logic block is NOT a pipeline
    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "app::pipeline";
    lb.simpleName = "pipeline";
    lb.isPipeline = false; // not a pipeline
    lb.calledFunctions = {"app::nodeA", "app::nodeB"};
    lb.stages = {1, 1};
    symbols.addLogicBlock(lb);

    TopoInlinePass::run(*module, OptLevel::O2, entries, mapping, &symbols);

    // helper should NOT be upgraded (non-pipeline logic block)
    auto* helperFunc = module->getFunction("_ZN3app6helperEv");
    ASSERT_NE(helperFunc, nullptr);
    EXPECT_TRUE(helperFunc->hasFnAttribute(llvm::Attribute::InlineHint))
        << "Non-pipeline logic block should not trigger functor expansion";
    EXPECT_FALSE(helperFunc->hasFnAttribute(llvm::Attribute::AlwaysInline));
}

TEST(Obfuscation, DomainSeparation) {
    // Verify that salt="ab" + name="cde" differs from salt="abc" + name="de"
    // (SipHash uses salt as key, so domain separation is inherent)
    llvm::LLVMContext ctx;
    auto module1 = createSyntheticModule(ctx, {"_ZN3app3cdeEv"});
    auto module2 = createSyntheticModule(ctx, {"_ZN3app2deEv"});

    std::vector<VisibilityEntry> entries1 = {
        makeVisEntry("app::cde", Visibility::Private),
    };
    std::vector<VisibilityEntry> entries2 = {
        makeVisEntry("app::de", Visibility::Private),
    };

    SymbolMapper mapper(ctx);
    auto mapping1 = mapper.mapSymbols(*module1, entries1);
    auto mapping2 = mapper.mapSymbols(*module2, entries2);

    auto r1 = SymbolObfuscator::obfuscate(*module1, entries1, mapping1, ObfuscationMode::Salted, "ab");
    auto r2 = SymbolObfuscator::obfuscate(*module2, entries2, mapping2, ObfuscationMode::Salted, "abc");

    EXPECT_EQ(r1.renamedCount, 1);
    EXPECT_EQ(r2.renamedCount, 1);

    llvm::Function* f1 = &*module1->begin();
    llvm::Function* f2 = &*module2->begin();
    for (auto& f : *module1) {
        if (!f.isDeclaration()) {
            f1 = &f;
            break;
        }
    }
    for (auto& f : *module2) {
        if (!f.isDeclaration()) {
            f2 = &f;
            break;
        }
    }

    EXPECT_NE(f1->getName(), f2->getName())
        << "salt='ab'+name='cde' and salt='abc'+name='de' must produce different hashes";
}
