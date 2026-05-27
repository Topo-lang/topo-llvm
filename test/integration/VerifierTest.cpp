#include "topo/Basic/Diagnostic.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Backend/Verifier.h"
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

static std::unique_ptr<TopoFile> parseTopo(const std::string& source, DiagnosticEngine& diag) {
    Lexer lexer(source, "<test>", diag);
    Parser parser(lexer, diag);
    return parser.parseTopoFile();
}

// Helper: create synthetic module with given Itanium-mangled function names
// Optionally, some functions can call others (for fn block consistency check)
static std::unique_ptr<llvm::Module> createModule(llvm::LLVMContext& ctx,
                                                  const std::vector<std::string>& mangledNames) {
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    for (const auto& name : mangledNames) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, *module);
        llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(&func->getEntryBlock());
        builder.CreateRetVoid();
    }
    return module;
}

// Helper: create module where a function calls other functions
static std::unique_ptr<llvm::Module> createModuleWithCalls(llvm::LLVMContext& ctx,
                                                           const std::string& callerName,
                                                           const std::vector<std::string>& calleeNames,
                                                           const std::vector<std::string>& otherFunctions) {
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    // Create callees first
    std::vector<llvm::Function*> callees;
    for (const auto& name : calleeNames) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, *module);
        llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(&func->getEntryBlock());
        builder.CreateRetVoid();
        callees.push_back(func);
    }

    // Create the caller with call instructions
    auto* caller = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, callerName, *module);
    llvm::BasicBlock::Create(ctx, "entry", caller);
    llvm::IRBuilder<> builder(&caller->getEntryBlock());
    for (auto* callee : callees) {
        builder.CreateCall(callee);
    }
    builder.CreateRetVoid();

    // Create other standalone functions
    for (const auto& name : otherFunctions) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, *module);
        llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> b(&func->getEntryBlock());
        b.CreateRetVoid();
    }

    return module;
}

// --- All public symbols present → passed ---

TEST(Verifier, AllPublicPresent) {
    llvm::LLVMContext ctx;
    // run() calls init() and process() — matches the fn block in demo.topo
    auto module = createModuleWithCalls(ctx, "_ZN3app3runEv", {"_ZN3app4initEv", "_ZN3app7processEv"}, {});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    Verifier verifier(diag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_TRUE(result.passed());
    EXPECT_EQ(result.publicMissing, 0);
}

// --- Missing public symbol → publicMissing = 1 ---

TEST(Verifier, MissingPublicSymbol) {
    llvm::LLVMContext ctx;
    // Only provide init and process — run is missing
    auto module = createModule(ctx, {"_ZN3app4initEv", "_ZN3app7processEv"});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_FALSE(result.passed());
    EXPECT_EQ(result.publicMissing, 1);
}

// --- fn block operations match IR calls → passed ---

TEST(Verifier, FnBlockMatchesIRCalls) {
    llvm::LLVMContext ctx;
    // run() calls init() and process() — matches the fn block in demo.topo
    auto module = createModuleWithCalls(ctx, "_ZN3app3runEv", {"_ZN3app4initEv", "_ZN3app7processEv"}, {});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    Verifier verifier(diag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.publicMissing, 0);
    EXPECT_EQ(result.blockMismatches, 0);
}

// --- fn block declares extra operation → warning ---

TEST(Verifier, FnBlockExtraDeclaration) {
    llvm::LLVMContext ctx;
    // run() does NOT call anything — but fn block declares init(), process()
    auto module = createModule(ctx, {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    Verifier verifier(diag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.publicMissing, 0);
    // fn block declares init() and process() but IR doesn't call them
    EXPECT_GT(result.blockMismatches, 0);
}

// --- IR calls extra Topo symbol → warning ---

TEST(Verifier, IRCallsExtraTopoSymbol) {
    llvm::LLVMContext ctx;
    // run() calls init(), process(), AND itself (extra call)
    // Actually, let's test: run() calls init(), process() (matches) + extra
    // We need a topo source with a fn block that only has init() but IR calls
    // init() and process()
    auto source =
        "namespace app {\n"
        "  public:\n"
        "    void run();\n"
        "    fn run {\n"
        "      stage<1> init();\n"
        "    }\n"
        "  protected:\n"
        "    void init();\n"
        "    void process();\n"
        "}";

    // run() calls both init() and process()
    auto module = createModuleWithCalls(ctx, "_ZN3app3runEv", {"_ZN3app4initEv", "_ZN3app7processEv"}, {});

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    Verifier verifier(diag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.publicMissing, 0);
    // IR calls process() but fn block doesn't declare it
    EXPECT_GT(result.blockMismatches, 0);
}

// --- Protected missing does NOT trigger public check ---

TEST(Verifier, ProtectedMissingNoPublicError) {
    llvm::LLVMContext ctx;
    // Only provide run (public) — init and process (protected) are missing
    auto module = createModule(ctx, {"_ZN3app3runEv"});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    // public run() is present → no public missing
    EXPECT_EQ(result.publicMissing, 0);
}

// ===== Issue 005: Signature verification =====

// --- Matching signatures pass verification ---

TEST(VerifierSignature, MatchingSignaturesPasses) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    // Topo: int compute(int x, int y);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty, i32Ty}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN4math7computeEii", *module);
    func->getArg(0)->setName("x");
    func->getArg(1)->setName("y");
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));

    auto source =
        "using int = std::cpp17::int;\n"
        "namespace math {\n"
        "  public:\n"
        "    int compute(int x, int y);\n"
        "}";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.signatureMismatches, 0);
    EXPECT_TRUE(result.passed());
}

// --- Wrong return type triggers signatureMismatch ---

TEST(VerifierSignature, WrongReturnType) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    // IR: void compute() — but Topo says int compute()
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN4math7computeEv", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRetVoid();

    auto source =
        "using int = std::cpp17::int;\n"
        "namespace math {\n"
        "  public:\n"
        "    int compute();\n"
        "}";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_GT(result.signatureMismatches, 0) << "Return type mismatch (int vs void) should be detected";
}

// --- Wrong parameter count triggers signatureMismatch ---

TEST(VerifierSignature, WrongParamCount) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    // IR: void run(i32) — but Topo says void run()
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, {i32Ty}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app3runEi", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRetVoid();

    auto source =
        "namespace app {\n"
        "  public:\n"
        "    void run();\n"
        "}";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_GT(result.signatureMismatches, 0) << "Parameter count mismatch should be detected";
}

// --- Void function with matching void IR passes ---

TEST(VerifierSignature, VoidReturnMatches) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN3app3runEv", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRetVoid();

    auto source =
        "namespace app {\n"
        "  public:\n"
        "    void run();\n"
        "}";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.signatureMismatches, 0);
    EXPECT_TRUE(result.passed());
}

// ===== Issue 005: Class member verification =====

// Helper: create module with typed functions
static std::unique_ptr<llvm::Module> createModuleWithTypedFuncs(
    llvm::LLVMContext& ctx, const std::vector<std::pair<std::string, llvm::FunctionType*>>& funcs) {
    auto module = std::make_unique<llvm::Module>("test_module", ctx);
    for (const auto& [name, ty] : funcs) {
        auto* func = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, *module);
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(bb);
        if (ty->getReturnType()->isVoidTy()) {
            builder.CreateRetVoid();
        } else {
            builder.CreateRet(llvm::Constant::getNullValue(ty->getReturnType()));
        }
    }
    return module;
}

// --- All class members present → passed ---

TEST(VerifierClassMember, ClassMemberPresent) {
    llvm::LLVMContext ctx;
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    // Foo::bar(this) → void
    auto* barTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
    // Foo::baz(this, int) → int
    auto* bazTy = llvm::FunctionType::get(i32Ty, {ptrTy, i32Ty}, false);
    // Foo::helper(this) → void
    auto* helperTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);

    auto module = createModuleWithTypedFuncs(
        ctx, {{"_ZN3app3Foo3barEv", barTy}, {"_ZN3app3Foo3bazEi", bazTy}, {"_ZN3app3Foo6helperEv", helperTy}});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/class_verify.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.classMemberMissing, 0);
}

// --- Public class member missing → classMemberMissing > 0 ---

TEST(VerifierClassMember, ClassMemberMissing) {
    llvm::LLVMContext ctx;
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    // Only provide helper — bar and baz (public) are missing
    auto* helperTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);

    auto module = createModuleWithTypedFuncs(ctx, {{"_ZN3app3Foo6helperEv", helperTy}});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/class_verify.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    // bar() and baz() are public members → classMemberMissing should be > 0
    EXPECT_GT(result.classMemberMissing, 0);
}

// --- fn block inconsistency is now error level (blockMismatches > 0) ---

TEST(VerifierFnBlock, FnBlockMismatchIsError) {
    llvm::LLVMContext ctx;
    // run() does NOT call init() — but fn block declares it
    auto module = createModule(ctx, {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    // fn block mismatches are now errors, blocking verification
    EXPECT_GT(result.blockMismatches, 0);
    EXPECT_FALSE(result.passed());

    // Verify that errors were emitted (not just warnings)
    EXPECT_TRUE(verifyDiag.hasErrors());
}

// ===== Issue 005: Stage ordering verification =====

// --- Stage order respected → stageOrderViolations == 0 ---

TEST(VerifierStageOrder, StageOrderRespected) {
    llvm::LLVMContext ctx;

    // demo.topo: fn run { stage<1> init(); stage<2> process(); }
    // IR calls init() before process() → correct order
    auto module = createModuleWithCalls(ctx, "_ZN3app3runEv", {"_ZN3app4initEv", "_ZN3app7processEv"}, {});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.stageOrderViolations, 0) << "Calling stage-1 before stage-2 should respect ordering";
}

// --- Stage order violated → stageOrderViolations > 0 ---

TEST(VerifierStageOrder, StageOrderViolated) {
    llvm::LLVMContext ctx;

    // demo.topo: fn run { stage<1> init(); stage<2> process(); }
    // IR calls process() before init() → WRONG order
    auto module = createModuleWithCalls(ctx, "_ZN3app3runEv", {"_ZN3app7processEv", "_ZN3app4initEv"}, {});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_GT(result.stageOrderViolations, 0) << "Calling stage-2 before stage-1 should violate ordering";
}

// ===== Issue 005: Pipeline edge verification =====

// --- Pipeline terminal correct → pipelineEdgeMismatches == 0 ---

TEST(VerifierPipelineEdge, PipelineTerminalCorrect) {
    llvm::LLVMContext ctx;

    // pipeline.topo: fn run { fetch->decode; decode->execute; execute->void; }
    // IR calls fetch, decode, execute in order → execute is last (terminal) → OK
    auto module = createModuleWithCalls(
        ctx, "_ZN8pipeline3runEv", {"_ZN8pipeline5fetchEv", "_ZN8pipeline6decodeEii", "_ZN8pipeline7executeEbi"}, {});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/pipeline.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.pipelineEdgeMismatches, 0) << "Terminal node execute should be the last called pipeline node";
}

// --- Pipeline terminal wrong → pipelineEdgeMismatches > 0 ---

TEST(VerifierPipelineEdge, PipelineTerminalWrong) {
    llvm::LLVMContext ctx;

    // pipeline.topo: fn run { fetch->decode; decode->execute; execute->void; }
    // IR calls fetch, execute, decode → decode is last, but execute is terminal → ERROR
    auto module = createModuleWithCalls(
        ctx, "_ZN8pipeline3runEv", {"_ZN8pipeline5fetchEv", "_ZN8pipeline7executeEbi", "_ZN8pipeline6decodeEii"}, {});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/pipeline.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_GT(result.pipelineEdgeMismatches, 0) << "Terminal node execute is not the last call — decode comes after it";
}

// ===== Template instantiation verification =====

// --- Instantiation present in IR → templateInstantiationMissing == 0 ---

TEST(VerifierTemplate, InstantiationPresentInIR) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    // Create functions that look like Vector<int>::push_back and Vector<int>::get
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* pushTy = llvm::FunctionType::get(voidTy, {ptrTy, i32Ty}, false);
    auto* getTy = llvm::FunctionType::get(i32Ty, {ptrTy, i32Ty}, false);
    auto* identityTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* insertTy = llvm::FunctionType::get(voidTy, {ptrTy, i32Ty, ptrTy}, false);

    // Itanium-mangled names for Vector<int> members, identity<int>, Map<int,double>
    std::vector<std::pair<std::string, llvm::FunctionType*>> funcs = {
        {"_ZN9container6VectorIiE9push_backEi", pushTy},
        {"_ZN9container6VectorIiE3getEi", getTy},
        {"_ZN9container8identityIiEET_S1_", identityTy},
        {"_ZN9container3MapIidE6insertEid", insertTy},
    };

    for (const auto& [name, ty] : funcs) {
        auto* func = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, *module);
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(bb);
        if (ty->getReturnType()->isVoidTy())
            builder.CreateRetVoid();
        else
            builder.CreateRet(llvm::Constant::getNullValue(ty->getReturnType()));
    }

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/template_instantiate.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    // May have warnings about constraint, but no hard errors from the fixture
    // Check that instantiates were stored
    ASSERT_GE(symbols.instantiates().size(), 1u);

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.templateInstantiationMissing, 0) << "All instantiated templates should be found in IR";
}

// --- Instantiation missing from IR → templateInstantiationMissing > 0 ---

TEST(VerifierTemplate, InstantiationMissingFromIR) {
    llvm::LLVMContext ctx;
    // Empty module — no template instantiations
    auto module = createModule(ctx, {});

    auto source = R"(
using Int = std::cpp17::int;
namespace container {
    public:
        template<typename T>
        class Vector {
            public:
                void push_back(T value);
        }
        instantiate Vector<Int>;
})";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_GT(result.templateInstantiationMissing, 0) << "Missing instantiation should be detected";
}

// --- Function template instantiation ---

TEST(VerifierTemplate, FunctionTemplateInstantiation) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* func =
        llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN4math3maxIiEET_S1_S1_", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRet(llvm::Constant::getNullValue(i32Ty));

    auto source = R"(
using Int = std::cpp17::int;
namespace math {
    public:
        template<typename T>
        T max(T a, T b);
        instantiate max<Int>;
})";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.templateInstantiationMissing, 0) << "Function template instantiation should be found";
}

// --- Multi-argument instantiation ---

TEST(VerifierTemplate, MultiArgInstantiation) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* f64Ty = llvm::Type::getDoubleTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, {ptrTy, i32Ty, f64Ty}, false);
    auto* func =
        llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "_ZN9container3MapIidE6insertEid", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRetVoid();

    auto source = R"(
using Int = std::cpp17::int;
using Double = std::cpp17::double;
namespace container {
    public:
        template<typename K, typename V>
        class Map {
            public:
                void insert(K key, V value);
        }
        instantiate Map<Int, Double>;
})";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.templateInstantiationMissing, 0) << "Multi-arg template instantiation should be found";
}

// ===== Constraint satisfaction verification =====

// --- Adapted type all members present → constraintViolations == 0 ---

TEST(VerifierConstraint, AdaptedTypeAllMembersPresent) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i1Ty = llvm::Type::getInt1Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    // Create adapt target functions: int_add, int_zero, int_equal, Container<int>::push
    auto* addTy = llvm::FunctionType::get(i32Ty, {i32Ty, i32Ty}, false);
    auto* zeroTy = llvm::FunctionType::get(i32Ty, false);
    auto* equalTy = llvm::FunctionType::get(i1Ty, {i32Ty, i32Ty}, false);
    auto* pushTy = llvm::FunctionType::get(voidTy, {ptrTy, i32Ty}, false);

    std::vector<std::pair<std::string, llvm::FunctionType*>> funcs = {
        {"_ZN4math7int_addEii", addTy},
        {"_ZN4math8int_zeroEv", zeroTy},
        {"_ZN4math9int_equalEii", equalTy},
        {"_ZN4math9ContainerIiE4pushEi", pushTy},
    };

    for (const auto& [name, ty] : funcs) {
        auto* func = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, *module);
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(bb);
        if (ty->getReturnType()->isVoidTy())
            builder.CreateRetVoid();
        else
            builder.CreateRet(llvm::Constant::getNullValue(ty->getReturnType()));
    }

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/constraint_verify.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.constraintViolations, 0) << "All adapt target functions exist — no violations";
}

// --- Adapted type missing function → constraintViolations > 0 ---

TEST(VerifierConstraint, AdaptedTypeMissingFunction) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    auto* i32Ty = llvm::Type::getInt32Ty(ctx);

    // Only create int_add — int_zero and int_equal missing
    auto* addTy = llvm::FunctionType::get(i32Ty, {i32Ty, i32Ty}, false);
    auto* func = llvm::Function::Create(addTy, llvm::GlobalValue::ExternalLinkage, "_ZN4math7int_addEii", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRet(llvm::Constant::getNullValue(i32Ty));

    auto source = R"(
using Int = std::cpp17::int;
using Bool = std::cpp17::bool;
namespace math {
    public:
        constraint Numeric {
            T add(T a, T b);
            T zero();
        }
        adapt Numeric for Int {
            add = int_add;
            zero = int_zero;
        }
        Int int_add(Int a, Int b);
        Int int_zero();
        template<typename T : Numeric>
        class Box {
            public:
                void store(T value);
        }
        instantiate Box<Int>;
})";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_GT(result.constraintViolations, 0) << "int_zero mapping target missing from IR → violation";
}

// --- Constrained instantiation without adapt → warning ---

TEST(VerifierConstraint, ConstrainedInstantiationWithoutAdapt) {
    llvm::LLVMContext ctx;
    auto module = createModule(ctx, {});

    auto source = R"(
using Double = std::cpp17::double;
namespace math {
    public:
        constraint Sortable {
            Bool less(T a, T b);
        }
        template<typename T : Sortable>
        void sort(T data);
        instantiate sort<Double>;
})";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    // No adapt for Double → Sortable: should produce warning (not error)
    EXPECT_EQ(result.constraintViolations, 0) << "Missing adapt is a warning, not an error";
    // But the verifyDiag should have a warning in its diagnostics
    bool hasWarning = false;
    for (const auto& d : verifyDiag.diagnostics()) {
        if (d.level == DiagLevel::Warning) {
            hasWarning = true;
            break;
        }
    }
    EXPECT_TRUE(hasWarning) << "Should warn about missing adapt";
}

// --- Inherited constraint members also verified ---

TEST(VerifierConstraint, InheritedConstraintMembers) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i1Ty = llvm::Type::getInt1Ty(ctx);

    // Create int_add and int_equal (from parent Comparable) — but NOT int_less
    std::vector<std::pair<std::string, llvm::FunctionType*>> funcs = {
        {"_ZN4math7int_addEii", llvm::FunctionType::get(i32Ty, {i32Ty, i32Ty}, false)},
        {"_ZN4math9int_equalEii", llvm::FunctionType::get(i1Ty, {i32Ty, i32Ty}, false)},
    };

    for (const auto& [name, ty] : funcs) {
        auto* func = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, *module);
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(bb);
        builder.CreateRet(llvm::Constant::getNullValue(ty->getReturnType()));
    }

    auto source = R"(
using Int = std::cpp17::int;
using Bool = std::cpp17::bool;
namespace math {
    public:
        constraint Comparable {
            Bool equal(T a, T b);
        }
        constraint Ordered : Comparable {
            Bool less(T a, T b);
            T add(T a, T b);
        }
        adapt Ordered for Int {
            less = int_less;
            add = int_add;
            equal = int_equal;
        }
        Int int_add(Int a, Int b);
        Bool int_less(Int a, Int b);
        Bool int_equal(Int a, Int b);
        template<typename T : Ordered>
        void sort(T data);
        instantiate sort<Int>;
})";

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    // int_less is missing from IR → constraint violation
    EXPECT_GT(result.constraintViolations, 0) << "Missing inherited constraint member target should be detected";
}

// ===== Stage parallel safety verification =====

// --- Independent same-stage operations → stageParallelViolations == 0 ---

TEST(VerifierStageParallel, IndependentSameStageOps) {
    llvm::LLVMContext ctx;

    // Both opA and opB are void() — no data flow between them
    auto module = createModuleWithCalls(
        ctx, "_ZN7compute3runEv", {"_ZN7compute3opAEv", "_ZN7compute3opBEv", "_ZN7compute8finalizeEv"}, {});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/stage_parallel.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.stageParallelViolations, 0) << "Independent void calls should have no data dependency";
}

// --- Direct data dependency between same-stage ops → stageParallelViolations > 0 ---

TEST(VerifierStageParallel, DirectDataDependency) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);

    // Create produce() → returns i32
    auto* produceTy = llvm::FunctionType::get(i32Ty, false);
    auto* produce =
        llvm::Function::Create(produceTy, llvm::GlobalValue::ExternalLinkage, "_ZN7compute7produceEv", *module);
    auto* pBB = llvm::BasicBlock::Create(ctx, "entry", produce);
    llvm::IRBuilder<> pBuilder(pBB);
    pBuilder.CreateRet(llvm::ConstantInt::get(i32Ty, 42));

    // Create consume(i32) → void  (takes produce's return value!)
    auto* consumeTy = llvm::FunctionType::get(voidTy, {i32Ty}, false);
    auto* consume =
        llvm::Function::Create(consumeTy, llvm::GlobalValue::ExternalLinkage, "_ZN7compute7consumeEv", *module);
    auto* cBB = llvm::BasicBlock::Create(ctx, "entry", consume);
    llvm::IRBuilder<> cBuilder(cBB);
    cBuilder.CreateRetVoid();

    // Create finalize() → void
    auto* finalizeTy = llvm::FunctionType::get(voidTy, false);
    auto* finalize =
        llvm::Function::Create(finalizeTy, llvm::GlobalValue::ExternalLinkage, "_ZN7compute8finalizeEv", *module);
    auto* fBB = llvm::BasicBlock::Create(ctx, "entry", finalize);
    llvm::IRBuilder<> fBuilder(fBB);
    fBuilder.CreateRetVoid();

    // Create run_dep() — calls produce, then passes result to consume
    auto* runTy = llvm::FunctionType::get(voidTy, false);
    auto* runDep = llvm::Function::Create(runTy, llvm::GlobalValue::ExternalLinkage, "_ZN7compute7run_depEv", *module);
    auto* rBB = llvm::BasicBlock::Create(ctx, "entry", runDep);
    llvm::IRBuilder<> rBuilder(rBB);
    auto* produceResult = rBuilder.CreateCall(produce);
    rBuilder.CreateCall(consume, {produceResult}); // Direct dependency!
    rBuilder.CreateCall(finalize);
    rBuilder.CreateRetVoid();

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/stage_parallel.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_GT(result.stageParallelViolations, 0) << "produce() return value feeds consume() — direct data dependency";
}

// --- Different-stage dependency is OK ---

TEST(VerifierStageParallel, DifferentStageDependencyOk) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_module", ctx);

    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);

    // solo() → i32, finish(i32) → void — different stages (1 and 2)
    auto* soloTy = llvm::FunctionType::get(i32Ty, false);
    auto* solo = llvm::Function::Create(soloTy, llvm::GlobalValue::ExternalLinkage, "_ZN7compute4soloEv", *module);
    auto* sBB = llvm::BasicBlock::Create(ctx, "entry", solo);
    llvm::IRBuilder<> sBuilder(sBB);
    sBuilder.CreateRet(llvm::ConstantInt::get(i32Ty, 1));

    auto* finishTy = llvm::FunctionType::get(voidTy, {i32Ty}, false);
    auto* finish =
        llvm::Function::Create(finishTy, llvm::GlobalValue::ExternalLinkage, "_ZN7compute6finishEv", *module);
    auto* fBB = llvm::BasicBlock::Create(ctx, "entry", finish);
    llvm::IRBuilder<> fBuilder(fBB);
    fBuilder.CreateRetVoid();

    // run_single() — solo (stage 1) result flows to finish (stage 2) → OK
    auto* runTy = llvm::FunctionType::get(voidTy, false);
    auto* runSingle =
        llvm::Function::Create(runTy, llvm::GlobalValue::ExternalLinkage, "_ZN7compute10run_singleEv", *module);
    auto* rBB = llvm::BasicBlock::Create(ctx, "entry", runSingle);
    llvm::IRBuilder<> rBuilder(rBB);
    auto* soloResult = rBuilder.CreateCall(solo);
    rBuilder.CreateCall(finish, {soloResult});
    rBuilder.CreateRetVoid();

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/stage_parallel.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.stageParallelViolations, 0) << "Data flow between different stages is legal";
}

// --- Single-op stage skips check ---

TEST(VerifierStageParallel, SingleOpStageNoCheck) {
    llvm::LLVMContext ctx;

    // run_single has stage<1> solo(); stage<2> finish(); — each stage has 1 op
    auto module =
        createModuleWithCalls(ctx, "_ZN7compute10run_singleEv", {"_ZN7compute4soloEv", "_ZN7compute6finishEv"}, {});

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/stage_parallel.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(*module, VisibilityCollector().collect(static_cast<const TopoFile&>(*ast)));

    DiagnosticEngine verifyDiag;
    Verifier verifier(verifyDiag, symbols);
    auto result = verifier.verify(*module, mapping);

    EXPECT_EQ(result.stageParallelViolations, 0) << "Single-op stages should not trigger parallel safety violations";
}
