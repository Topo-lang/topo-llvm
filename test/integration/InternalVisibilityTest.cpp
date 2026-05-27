#include "topo/Basic/Diagnostic.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Backend/VisibilityApplier.h"
#include "topo/Sema/VisibilityCollector.h"
#include "topo/Lexer/Lexer.h"
#include "topo/Parser/Parser.h"
#include "topo/Sema/SemanticAnalyzer.h"

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ModRef.h>

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

static std::unique_ptr<TopoFile> parseTopo(const std::string& source, DiagnosticEngine& diag) {
    Lexer lexer(source, "<test>", diag);
    Parser parser(lexer, diag);
    return parser.parseTopoFile();
}

// --- internal_basic.topo: internal functions collected with Internal visibility ---

TEST(InternalVisibility, BasicInternalCollectedAsInternal) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/internal_basic.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    // internal_basic.topo: public run + fn run, internal init + process
    // All should be collected, but init/process with Internal visibility
    bool foundRun = false, foundInit = false, foundProcess = false;
    for (const auto& entry : entries) {
        if (entry.qualifiedName == "engine::run") {
            foundRun = true;
            EXPECT_EQ(entry.visibility, Visibility::Public);
        }
        if (entry.qualifiedName == "engine::init") {
            foundInit = true;
            EXPECT_EQ(entry.visibility, Visibility::Internal);
        }
        if (entry.qualifiedName == "engine::process") {
            foundProcess = true;
            EXPECT_EQ(entry.visibility, Visibility::Internal);
        }
    }
    EXPECT_TRUE(foundRun) << "public function 'run' should be in entries";
    EXPECT_TRUE(foundInit) << "internal function 'init' should be in entries";
    EXPECT_TRUE(foundProcess) << "internal function 'process' should be in entries";
}

// --- internal_namespace.topo: internal namespace entries marked Internal ---

TEST(InternalVisibility, NamespaceCollectedAsInternal) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/internal_namespace.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    // detail namespace is internal -> helper and secret should have Internal
    bool foundHelper = false, foundSecret = false, foundServe = false;
    for (const auto& entry : entries) {
        if (entry.qualifiedName == "detail::helper") {
            foundHelper = true;
            EXPECT_EQ(entry.visibility, Visibility::Internal);
        }
        if (entry.qualifiedName == "detail::secret") {
            foundSecret = true;
            EXPECT_EQ(entry.visibility, Visibility::Internal);
        }
        if (entry.qualifiedName == "api::serve") {
            foundServe = true;
            EXPECT_EQ(entry.visibility, Visibility::Public);
        }
    }
    EXPECT_TRUE(foundHelper) << "'detail::helper' should be in entries (as internal)";
    EXPECT_TRUE(foundSecret) << "'detail::secret' should be in entries (as internal)";
    EXPECT_TRUE(foundServe) << "'api::serve' should be in entries (as public)";
}

// --- SymbolMapper maps internal functions too (for linkage application) ---

TEST(InternalVisibility, SymbolMapperMapsInternal) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/internal_basic.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    // Create a synthetic LLVM module with all three functions
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    std::vector<std::string> mangledNames = {"_ZN6engine3runEv", "_ZN6engine4initEv", "_ZN6engine7processEv"};

    for (const auto& name : mangledNames) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, *module);
        llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(&func->getEntryBlock());
        builder.CreateRetVoid();
    }

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    // All three should be matched
    EXPECT_NE(mapping.matched.find("engine::run"), mapping.matched.end()) << "public function 'run' should be mapped";
    EXPECT_NE(mapping.matched.find("engine::init"), mapping.matched.end())
        << "internal function 'init' should be mapped";
    EXPECT_NE(mapping.matched.find("engine::process"), mapping.matched.end())
        << "internal function 'process' should be mapped";
}

// --- VisibilityApplier sets InternalLinkage + AlwaysInline for Internal ---

TEST(InternalVisibility, ApplierSetsInternalLinkage) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/internal_basic.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    // Create LLVM module
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    std::vector<std::string> mangledNames = {"_ZN6engine3runEv", "_ZN6engine4initEv", "_ZN6engine7processEv"};

    for (const auto& name : mangledNames) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, *module);
        llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(&func->getEntryBlock());
        builder.CreateRetVoid();
    }

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    VisibilityApplier applier;
    auto stats = applier.apply(*module, mapping, entries);

    EXPECT_EQ(stats.publicCount, 1);   // run
    EXPECT_EQ(stats.internalCount, 2); // init, process

    // Verify init and process have InternalLinkage + AlwaysInline
    auto* initFunc = mapping.matched.at("engine::init");
    EXPECT_EQ(initFunc->getLinkage(), llvm::GlobalValue::InternalLinkage);
    EXPECT_TRUE(initFunc->hasFnAttribute(llvm::Attribute::AlwaysInline));

    auto* processFunc = mapping.matched.at("engine::process");
    EXPECT_EQ(processFunc->getLinkage(), llvm::GlobalValue::InternalLinkage);
    EXPECT_TRUE(processFunc->hasFnAttribute(llvm::Attribute::AlwaysInline));

    // run should NOT have dso_local set (public, exe mode) —
    // linker resolves locally anyway, and dso_local shifts inlining heuristics.
    auto* runFunc = mapping.matched.at("engine::run");
    EXPECT_FALSE(runFunc->isDSOLocal());
}

// ===== --debug-internal: preserve debug info for internal symbols =====

TEST(InternalVisibility, DebugInternalPreservesDebugInfo) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/internal_basic.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    // Create LLVM module with debug info
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    // Set up debug info infrastructure
    auto* diBuilder = new llvm::DIBuilder(*module);
    auto* cu = diBuilder->createCompileUnit(
        llvm::dwarf::DW_LANG_C_plus_plus, diBuilder->createFile("test.cpp", "/tmp"), "topo-test", false, "", 0);
    auto* diType = diBuilder->createSubroutineType(diBuilder->getOrCreateTypeArray({}));

    std::vector<std::string> mangledNames = {"_ZN6engine3runEv", "_ZN6engine4initEv", "_ZN6engine7processEv"};

    for (const auto& name : mangledNames) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, *module);
        llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(&func->getEntryBlock());
        builder.CreateRetVoid();

        // Attach debug subprogram
        auto* sp = diBuilder->createFunction(cu,
                                             name,
                                             name,
                                             diBuilder->createFile("test.cpp", "/tmp"),
                                             1,
                                             diType,
                                             1,
                                             llvm::DINode::FlagZero,
                                             llvm::DISubprogram::SPFlagDefinition);
        func->setSubprogram(sp);
    }
    diBuilder->finalize();

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    // Apply with debugInternal = true
    VisibilityApplier applier;
    auto stats = applier.apply(*module, mapping, entries, OutputType::Exe, /*debugInternal=*/true);

    EXPECT_EQ(stats.internalCount, 2);

    // Internal functions should be renamed with __topo_internal_ prefix
    auto* initFunc = mapping.matched.at("engine::init");
    EXPECT_TRUE(initFunc->getName().starts_with("__topo_internal_"))
        << "init should have __topo_internal_ prefix, got: " << initFunc->getName().str();

    auto* processFunc = mapping.matched.at("engine::process");
    EXPECT_TRUE(processFunc->getName().starts_with("__topo_internal_"))
        << "process should have __topo_internal_ prefix, got: " << processFunc->getName().str();

    // Debug info should be preserved
    EXPECT_NE(initFunc->getSubprogram(), nullptr) << "debug-internal should preserve DISubprogram for init";
    EXPECT_NE(processFunc->getSubprogram(), nullptr) << "debug-internal should preserve DISubprogram for process";

    // Public function should be unchanged
    auto* runFunc = mapping.matched.at("engine::run");
    EXPECT_FALSE(runFunc->getName().starts_with("__topo_internal_"));

    delete diBuilder;
}

TEST(InternalVisibility, DefaultStripsDebugSubprogram) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/internal_basic.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    // Create LLVM module with debug info
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    auto* diBuilder = new llvm::DIBuilder(*module);
    auto* cu = diBuilder->createCompileUnit(
        llvm::dwarf::DW_LANG_C_plus_plus, diBuilder->createFile("test.cpp", "/tmp"), "topo-test", false, "", 0);
    auto* diType = diBuilder->createSubroutineType(diBuilder->getOrCreateTypeArray({}));

    std::vector<std::string> mangledNames = {"_ZN6engine3runEv", "_ZN6engine4initEv", "_ZN6engine7processEv"};

    for (const auto& name : mangledNames) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, *module);
        llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(&func->getEntryBlock());
        builder.CreateRetVoid();

        auto* sp = diBuilder->createFunction(cu,
                                             name,
                                             name,
                                             diBuilder->createFile("test.cpp", "/tmp"),
                                             1,
                                             diType,
                                             1,
                                             llvm::DINode::FlagZero,
                                             llvm::DISubprogram::SPFlagDefinition);
        func->setSubprogram(sp);
    }
    diBuilder->finalize();

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    // Apply with debugInternal = false (default)
    VisibilityApplier applier;
    auto stats = applier.apply(*module, mapping, entries);

    EXPECT_EQ(stats.internalCount, 2);

    // Internal functions should have debug info stripped
    auto* initFunc = mapping.matched.at("engine::init");
    EXPECT_EQ(initFunc->getSubprogram(), nullptr) << "default mode should strip DISubprogram for internal functions";

    auto* processFunc = mapping.matched.at("engine::process");
    EXPECT_EQ(processFunc->getSubprogram(), nullptr) << "default mode should strip DISubprogram for internal functions";

    // Function name should NOT have __topo_internal_ prefix
    EXPECT_FALSE(initFunc->getName().starts_with("__topo_internal_"));
    EXPECT_FALSE(processFunc->getName().starts_with("__topo_internal_"));

    // Public function should still have debug info
    auto* runFunc = mapping.matched.at("engine::run");
    EXPECT_NE(runFunc->getSubprogram(), nullptr) << "public function debug info should be preserved";

    delete diBuilder;
}

// ===== Issue 004: const propagation to LLVM IR =====

// --- VisibilityCollector extracts isConst for const member functions ---

TEST(ConstPropagation, CollectorExtractsConstFunction) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/const_test.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    bool foundGetValue = false;
    bool foundCompute = false;
    for (const auto& entry : entries) {
        if (entry.qualifiedName == "math::getValue") {
            foundGetValue = true;
            EXPECT_TRUE(entry.isConst) << "getValue should be marked const";
        }
        if (entry.qualifiedName == "math::compute") {
            foundCompute = true;
            EXPECT_FALSE(entry.isConst) << "compute should not be const";
            // Both params should be const ref
            ASSERT_EQ(entry.paramConsts.size(), 2u);
            EXPECT_TRUE(entry.paramConsts[0].isConst);
            EXPECT_EQ(entry.paramConsts[0].modifier, TypeNode::Ref);
            EXPECT_TRUE(entry.paramConsts[1].isConst);
            EXPECT_EQ(entry.paramConsts[1].modifier, TypeNode::Ref);
        }
    }
    EXPECT_TRUE(foundGetValue) << "getValue should be in entries";
    EXPECT_TRUE(foundCompute) << "compute should be in entries";
}

// --- VisibilityApplier sets memory(read) for const member functions ---

TEST(ConstPropagation, ApplierSetsMemoryReadForConstFunction) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/const_test.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    // Create LLVM module with matching functions
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    // getValue() const — no args, returns i32
    {
        auto* funcTy = llvm::FunctionType::get(i32Ty, false);
        auto* func =
            llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZNK4math8getValueEv", *module);
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(bb);
        builder.CreateRet(llvm::ConstantInt::get(i32Ty, 42));
    }

    // compute(const int&, const int&) — two i32* args, returns i32
    {
        auto* ptrTy = llvm::PointerType::getUnqual(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy}, false);
        auto* func =
            llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN4math7computeERKiRKi", *module);
        func->getArg(0)->setName("x");
        func->getArg(1)->setName("y");
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(bb);
        builder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    // update(int&) — one i32* arg, returns void (non-const param)
    {
        auto* ptrTy = llvm::PointerType::getUnqual(ctx);
        auto* funcTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN4math6updateERi", *module);
        func->getArg(0)->setName("data");
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(bb);
        builder.CreateRetVoid();
    }

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    VisibilityApplier applier;
    applier.apply(*module, mapping, entries);

    // getValue should have memory(read) effect
    auto* getValueFunc = mapping.matched.at("math::getValue");
    EXPECT_TRUE(getValueFunc->doesNotAccessMemory() == false);
    EXPECT_TRUE(getValueFunc->onlyReadsMemory()) << "const function getValue should have memory(read)";

    // compute's const ref params should have readonly attribute
    auto* computeFunc = mapping.matched.at("math::compute");
    EXPECT_TRUE(computeFunc->getArg(0)->hasAttribute(llvm::Attribute::ReadOnly))
        << "const ref param x should have readonly";
    EXPECT_TRUE(computeFunc->getArg(1)->hasAttribute(llvm::Attribute::ReadOnly))
        << "const ref param y should have readonly";

    // update's non-const param should NOT have readonly
    auto* updateFunc = mapping.matched.at("math::update");
    EXPECT_FALSE(updateFunc->getArg(0)->hasAttribute(llvm::Attribute::ReadOnly))
        << "non-const ref param data should not have readonly";
}

// ===== ComptimeIf: functions inside comptime if blocks are collected =====

TEST(VisibilityCollector, ComptimeIfFunctionsCollected) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/comptime_visibility.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    bool foundAlwaysVisible = false;
    bool foundComptimeTrue = false;
    bool foundComptimeFalse = false;
    for (const auto& entry : entries) {
        if (entry.qualifiedName == "test::always_visible") {
            foundAlwaysVisible = true;
            EXPECT_EQ(entry.visibility, Visibility::Public);
        }
        if (entry.qualifiedName == "test::comptime_true_func") {
            foundComptimeTrue = true;
            EXPECT_EQ(entry.visibility, Visibility::Public);
        }
        if (entry.qualifiedName == "test::comptime_false_func") {
            foundComptimeFalse = true;
            EXPECT_EQ(entry.visibility, Visibility::Public);
        }
    }
    EXPECT_TRUE(foundAlwaysVisible) << "always_visible should be collected";
    EXPECT_TRUE(foundComptimeTrue) << "comptime_true_func (then branch) should be collected";
    EXPECT_TRUE(foundComptimeFalse) << "comptime_false_func (else branch) should be collected";
}

// ===== Issue 022: O0 NoInline stripped alongside OptimizeNone =====

TEST(VisibilityApplier, StripsO0NoInline) {
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/internal_basic.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    auto entries = VisibilityCollector().collect(static_cast<const TopoFile&>(*ast));

    // Create LLVM module with functions pre-set with OptimizeNone + NoInline
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    std::vector<std::string> mangledNames = {"_ZN6engine3runEv", "_ZN6engine4initEv", "_ZN6engine7processEv"};

    for (const auto& name : mangledNames) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, *module);
        // Simulate O0 IR: OptimizeNone + NoInline (both from -O0)
        func->addFnAttr(llvm::Attribute::OptimizeNone);
        func->addFnAttr(llvm::Attribute::NoInline);
        llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(&func->getEntryBlock());
        builder.CreateRetVoid();
    }

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, entries);

    VisibilityApplier applier;
    applier.apply(*module, mapping, entries);

    // All functions: both O0 blocker attributes should be removed
    for (const auto& name : mangledNames) {
        auto* func = module->getFunction(name);
        ASSERT_NE(func, nullptr);
        EXPECT_FALSE(func->hasFnAttribute(llvm::Attribute::OptimizeNone))
            << "OptimizeNone should be stripped from " << name;
        EXPECT_FALSE(func->hasFnAttribute(llvm::Attribute::NoInline)) << "O0 NoInline should be stripped from " << name;
    }

    // Internal functions should still get InternalLinkage
    auto* initFunc = mapping.matched.at("engine::init");
    EXPECT_EQ(initFunc->getLinkage(), llvm::GlobalValue::InternalLinkage);
    auto* processFunc = mapping.matched.at("engine::process");
    EXPECT_EQ(processFunc->getLinkage(), llvm::GlobalValue::InternalLinkage);
}
