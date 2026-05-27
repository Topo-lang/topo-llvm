#include "topo/Backend/IRTransformBackend.h"
#include "topo/Backend/LLVMTransformBackend.h"
#include "topo/Basic/Diagnostic.h"
#include "topo/Lexer/Lexer.h"
#include "topo/Parser/Parser.h"
#include "topo/Sema/SemanticAnalyzer.h"
#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace topo;

#ifdef _WIN32
#include <process.h>
static int topo_getpid() {
    return _getpid();
}
#else
#include <unistd.h>
static int topo_getpid() {
    return getpid();
}
#endif

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

// Write a minimal LLVM IR file for testing
static std::string writeTestIR(const fs::path& dir,
                               const std::string& name,
                               const std::vector<std::string>& funcNames) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>(name, ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, false);

    for (const auto& fn : funcNames) {
        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, fn, *module);
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> builder(bb);
        builder.CreateRetVoid();
    }

    auto path = (dir / (name + ".ll")).string();
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec);
    if (!ec) module->print(out, nullptr);
    return path;
}

class LLVMTransformBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = fs::temp_directory_path() / ("topo-backend-test_" + std::to_string(topo_getpid()));
        fs::create_directories(tempDir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
    }

    fs::path tempDir_;
};

TEST_F(LLVMTransformBackendTest, LoadAndLinkSingleFile) {
    auto irPath = writeTestIR(tempDir_, "mod1", {"_Z3runv", "_Z4initv"});
    auto backend = backend::createLLVMBackend();
    ASSERT_TRUE(backend->loadAndLink({irPath}));
}

TEST_F(LLVMTransformBackendTest, LoadAndLinkMultipleFiles) {
    auto ir1 = writeTestIR(tempDir_, "mod1", {"_Z3runv"});
    auto ir2 = writeTestIR(tempDir_, "mod2", {"_Z4initv"});
    auto backend = backend::createLLVMBackend();
    ASSERT_TRUE(backend->loadAndLink({ir1, ir2}));
}

TEST_F(LLVMTransformBackendTest, LoadAndLinkInvalidPath) {
    auto backend = backend::createLLVMBackend();
    ASSERT_FALSE(backend->loadAndLink({"/nonexistent/path.ll"}));
}

TEST_F(LLVMTransformBackendTest, LoadAndLinkEmpty) {
    auto backend = backend::createLLVMBackend();
    ASSERT_FALSE(backend->loadAndLink({}));
}

TEST_F(LLVMTransformBackendTest, MapSymbolsReturnsCorrectCounts) {
    // Itanium-mangled: app::run() -> _ZN3app3runEv
    //                  app::init() -> _ZN3app4initEv
    //                  app::process() -> _ZN3app7processEv
    auto irPath = writeTestIR(tempDir_, "demo", {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    auto backend = backend::createLLVMBackend();
    ASSERT_TRUE(backend->loadAndLink({irPath}));

    // Parse demo.topo to get visibility entries
    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    VisibilityCollector collector;
    auto visEntries = collector.collect(static_cast<const TopoFile&>(*ast));
    ASSERT_EQ(visEntries.size(), 3u);

    auto result = backend->mapSymbols(visEntries);
    EXPECT_EQ(result.matched.size(), 3u);
    EXPECT_TRUE(result.unmatchedTopo.empty());
}

TEST_F(LLVMTransformBackendTest, VerifyReturnsResult) {
    auto irPath = writeTestIR(tempDir_, "demo", {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    auto backend = backend::createLLVMBackend();
    ASSERT_TRUE(backend->loadAndLink({irPath}));

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    VisibilityCollector collector;
    auto visEntries = collector.collect(static_cast<const TopoFile&>(*ast));

    backend->mapSymbols(visEntries);

    DiagnosticEngine verifyDiag;
    auto vr = backend->verify(verifyDiag, symbols, visEntries);
    // With synthetic IR (void() functions), we expect signature mismatches
    // but the call should succeed without crashing
    (void)vr;
}

TEST_F(LLVMTransformBackendTest, WriteIRProducesFile) {
    auto irPath = writeTestIR(tempDir_, "mod1", {"_Z3runv"});
    auto backend = backend::createLLVMBackend();
    ASSERT_TRUE(backend->loadAndLink({irPath}));

    auto outDir = (tempDir_ / "output").string();
    fs::create_directories(outDir);

    auto resultPath = backend->writeIR(outDir);
    ASSERT_FALSE(resultPath.empty());
    EXPECT_TRUE(fs::exists(resultPath));
    EXPECT_GT(fs::file_size(resultPath), 0u);
}

TEST_F(LLVMTransformBackendTest, DumpIRCopiesFile) {
    auto irPath = writeTestIR(tempDir_, "mod1", {"_Z3runv"});
    auto backend = backend::createLLVMBackend();
    ASSERT_TRUE(backend->loadAndLink({irPath}));

    auto outDir = (tempDir_ / "output").string();
    fs::create_directories(outDir);
    backend->writeIR(outDir);

    auto dumpPath = (tempDir_ / "dump.ll").string();
    ASSERT_TRUE(backend->dumpIR(dumpPath));
    EXPECT_TRUE(fs::exists(dumpPath));
}

TEST_F(LLVMTransformBackendTest, FullPipelineIntegration) {
    auto irPath = writeTestIR(tempDir_, "demo", {"_ZN3app3runEv", "_ZN3app4initEv", "_ZN3app7processEv"});

    auto backend = backend::createLLVMBackend();
    ASSERT_TRUE(backend->loadAndLink({irPath}));

    std::string source = readFile(TOPO_TEST_FIXTURES_DIR "/demo.topo");
    ASSERT_FALSE(source.empty());

    DiagnosticEngine diag;
    auto ast = parseTopo(source, diag);
    ASSERT_FALSE(diag.hasErrors());

    SemanticAnalyzer sema(diag);
    auto symbols = sema.analyze(static_cast<const TopoFile&>(*ast));
    ASSERT_FALSE(diag.hasErrors());

    VisibilityCollector collector;
    auto visEntries = collector.collect(static_cast<const TopoFile&>(*ast));

    // Step 5a: map
    auto mapping = backend->mapSymbols(visEntries);
    EXPECT_EQ(mapping.matched.size(), 3u);

    // Step 5b: verify
    DiagnosticEngine verifyDiag;
    backend->verify(verifyDiag, symbols, visEntries);

    // Step 6: optimize
    backend::BackendConfig config;
    config.optLevel = OptLevel::O2;
    config.buildMode = BuildMode::Dev;
    config.outputType = OutputType::Exe;
    config.obfMode = ObfuscationMode::Normal;

    auto result = backend->optimize(config, symbols, visEntries);
    // Check stats are populated
    int totalVis = result.visibilityStats.publicCount + result.visibilityStats.protectedCount +
                   result.visibilityStats.privateCount + result.visibilityStats.internalCount;
    EXPECT_GT(totalVis, 0);

    // Write IR
    auto outDir = (tempDir_ / "output").string();
    fs::create_directories(outDir);
    auto outPath = backend->writeIR(outDir);
    ASSERT_FALSE(outPath.empty());
    EXPECT_TRUE(fs::exists(outPath));
}
