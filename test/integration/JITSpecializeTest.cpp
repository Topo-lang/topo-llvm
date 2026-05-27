// Integration tests for real JIT specialization pipeline.
//
// Verifies the three bug fixes:
//   Bug 1: serializePreCodegenIR captures stubs with pipeline_placeholder
//   Bug 2: serializeMetadata includes calledFunctions field
//   Bug 3: PipelineCodeGenPass works on deserialized pre-codegen IR
//
// Also verifies edge pruning correctly removes unreachable nodes from
// the specialized pipeline.

#include "topo/Backend/IREmbed.h"
#include "topo/Backend/PassPipeline.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"
#include "topo/Transforms/PipelineCodeGenPass.h"
#include "topo/Transforms/TopoParallelPass.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/MemoryBuffer.h>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

using namespace topo;

// ============================================================
// MSVC-mangled name constants (platform: Windows + Clang MSVC ABI)
//   Itanium fallbacks tested below for cross-platform
// ============================================================

namespace {

// MSVC: int __cdecl topo::detail::pipeline_placeholder<int>(void)
constexpr const char* PLACEHOLDER_MSVC = "??$pipeline_placeholder@H@detail@topo@@YAHXZ";
// Itanium: int topo::detail::pipeline_placeholder<int>()
constexpr const char* PLACEHOLDER_ITANIUM = "_ZN4topo6detail20pipeline_placeholderIiEEiv";

// Choose whichever demangles correctly on this platform.
const char* pickPlaceholderName() {
    auto d = llvm::demangle(PLACEHOLDER_MSVC);
    if (d.find("topo::detail::pipeline_placeholder") != std::string::npos) return PLACEHOLDER_MSVC;
    d = llvm::demangle(PLACEHOLDER_ITANIUM);
    if (d.find("topo::detail::pipeline_placeholder") != std::string::npos) return PLACEHOLDER_ITANIUM;
    // Fallback — test will fail with a clear message
    return "topo_detail_pipeline_placeholder_UNKNOWN_MANGLING";
}

// MSVC-mangled stage function names for sim:: namespace
struct MangledNames {
    // int sim::tick(int)
    const char* tick;
    // int sim::stage_a(int)
    const char* stage_a;
    // int sim::stage_b(int)
    const char* stage_b;
    // int sim::stage_c(int, int)
    const char* stage_c;
};

MangledNames pickNames() {
    // Try MSVC first
    auto d = llvm::demangle("?tick@sim@@YAHH@Z");
    if (d.find("sim::tick") != std::string::npos) {
        return {
            "?tick@sim@@YAHH@Z",
            "?stage_a@sim@@YAHH@Z",
            "?stage_b@sim@@YAHH@Z",
            "?stage_c@sim@@YAHHH@Z",
        };
    }
    // Itanium
    return {
        "_ZN3sim4tickEi",
        "_ZN3sim7stage_aEi",
        "_ZN3sim7stage_bEi",
        "_ZN3sim7stage_cEii",
    };
}

} // anonymous namespace

// ============================================================
// Test fixture: creates an LLVM module with a pipeline stub
// and stage functions, plus corresponding SymbolTable/Mapping.
//
// Pipeline DAG:
//   stage_a(int) -> stage_b(int) -> stage_c(int, int)
//                \---> stage_c (second input)
//   sim::tick(int) is the pipeline function (stub)
//   tick_in is implicit (the pipeline param feeds stage_a)
// ============================================================

class JITSpecializeFixture : public ::testing::Test {
protected:
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    MangledNames names;

    void SetUp() override {
        names = pickNames();
        module = std::make_unique<llvm::Module>("jit_test", ctx);

        auto* i32 = llvm::Type::getInt32Ty(ctx);

        // --- Placeholder declaration ---
        auto* placeholderTy = llvm::FunctionType::get(i32, {}, false);
        llvm::Function::Create(placeholderTy, llvm::GlobalValue::ExternalLinkage, pickPlaceholderName(), *module);

        // --- Pipeline stub: sim::tick(int) ---
        // Body: return pipeline_placeholder<int>();
        auto* tickTy = llvm::FunctionType::get(i32, {i32}, false);
        auto* tickFunc = llvm::Function::Create(tickTy, llvm::GlobalValue::ExternalLinkage, names.tick, *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", tickFunc);
            llvm::IRBuilder<> b(bb);
            auto* ph = module->getFunction(pickPlaceholderName());
            auto* call = b.CreateCall(ph, {});
            b.CreateRet(call);
        }

        // --- Stage functions ---
        // stage_a(int x) { return x + 10; }
        auto* stageATy = llvm::FunctionType::get(i32, {i32}, false);
        auto* stageAFunc = llvm::Function::Create(stageATy, llvm::GlobalValue::ExternalLinkage, names.stage_a, *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageAFunc);
            llvm::IRBuilder<> b(bb);
            auto* sum = b.CreateAdd(stageAFunc->getArg(0), b.getInt32(10));
            b.CreateRet(sum);
        }

        // stage_b(int x) { return x + 20; }
        auto* stageBTy = llvm::FunctionType::get(i32, {i32}, false);
        auto* stageBFunc = llvm::Function::Create(stageBTy, llvm::GlobalValue::ExternalLinkage, names.stage_b, *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageBFunc);
            llvm::IRBuilder<> b(bb);
            auto* sum = b.CreateAdd(stageBFunc->getArg(0), b.getInt32(20));
            b.CreateRet(sum);
        }

        // stage_c(int a, int b) { return a + b + 100; }
        auto* stageCTy = llvm::FunctionType::get(i32, {i32, i32}, false);
        auto* stageCFunc = llvm::Function::Create(stageCTy, llvm::GlobalValue::ExternalLinkage, names.stage_c, *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageCFunc);
            llvm::IRBuilder<> b(bb);
            auto* sum1 = b.CreateAdd(stageCFunc->getArg(0), stageCFunc->getArg(1));
            auto* sum2 = b.CreateAdd(sum1, b.getInt32(100));
            b.CreateRet(sum2);
        }

        // --- SymbolTable ---
        LogicBlockEntry lb;
        lb.qualifiedName = "sim::tick";
        lb.simpleName = "tick";
        lb.isPipeline = true;
        lb.calledFunctions = {"sim::stage_a", "sim::stage_b", "sim::stage_c"};
        lb.edges = {
            {"stage_a", "stage_c", "", false, {}},
            {"stage_b", "stage_c", "", false, {}},
            {"stage_c", "", "std::cpp17::int", true, {}},
        };

        PipelineAnalysis analysis;
        analysis.stages = {
            {"stage_a", 0},
            {"stage_b", 0},
            {"stage_c", 1},
        };
        analysis.sourceNodes = {"stage_a", "stage_b"};
        analysis.terminalNode = "stage_c";
        analysis.terminalType = "int";
        lb.pipelineAnalysis = analysis;

        symbols.addLogicBlock(lb);

        // --- SymbolMapping ---
        mapping.matched["sim::tick"] = tickFunc;
        mapping.matched["sim::stage_a"] = stageAFunc;
        mapping.matched["sim::stage_b"] = stageBFunc;
        mapping.matched["sim::stage_c"] = stageCFunc;
    }
};

// ============================================================
// Bug 1: serializePreCodegenIR captures pipeline stubs
// ============================================================

TEST_F(JITSpecializeFixture, PreCodegenIR_CapturesPipelineStub) {
    auto bitcode = IREmbed::serializePreCodegenIR(*module, symbols, mapping);
    ASSERT_FALSE(bitcode.empty()) << "serializePreCodegenIR returned empty";

    // Parse the bitcode back
    auto buf = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char*>(bitcode.data()), bitcode.size()), "", false);
    llvm::LLVMContext ctx2;
    auto modOrErr = llvm::parseBitcodeFile(buf->getMemBufferRef(), ctx2);
    ASSERT_TRUE(!!modOrErr) << "Failed to parse pre-codegen bitcode";

    auto& restored = *modOrErr.get();

    // The pipeline stub must be present with a body
    auto* tickFunc = restored.getFunction(names.tick);
    ASSERT_NE(tickFunc, nullptr) << "Pipeline stub not in pre-codegen IR";
    EXPECT_FALSE(tickFunc->isDeclaration()) << "Pipeline stub should have a body";

    // The stub body must contain a call to pipeline_placeholder
    bool hasPlaceholder = false;
    for (auto& BB : *tickFunc) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = call->getCalledFunction()) {
                    auto demangled = llvm::demangle(callee->getName().str());
                    if (demangled.find("topo::detail::pipeline_placeholder") != std::string::npos) {
                        hasPlaceholder = true;
                    }
                }
            }
        }
    }
    EXPECT_TRUE(hasPlaceholder) << "Pre-codegen stub must still contain pipeline_placeholder call";

    // Stage functions must also be present
    EXPECT_NE(restored.getFunction(names.stage_a), nullptr) << "stage_a missing from pre-codegen IR";
    EXPECT_NE(restored.getFunction(names.stage_b), nullptr) << "stage_b missing from pre-codegen IR";
    EXPECT_NE(restored.getFunction(names.stage_c), nullptr) << "stage_c missing from pre-codegen IR";
}

TEST_F(JITSpecializeFixture, PreCodegenIR_VsPostCodegen) {
    // Pre-codegen: stub has placeholder call
    auto preCodegen = IREmbed::serializePreCodegenIR(*module, symbols, mapping);
    ASSERT_FALSE(preCodegen.empty());

    // Run PipelineCodeGenPass — replaces stub body
    int generated = PipelineCodeGenPass::run(*module, symbols, mapping);
    EXPECT_GE(generated, 1) << "PipelineCodeGenPass should generate at least 1 body";

    // Post-codegen: stub body replaced (no more placeholder)
    auto postCodegen = IREmbed::serializePipelineIR(*module, symbols, mapping);
    ASSERT_FALSE(postCodegen.empty());

    // Parse post-codegen, verify NO placeholder call
    auto buf = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char*>(postCodegen.data()), postCodegen.size()), "", false);
    llvm::LLVMContext ctx2;
    auto modOrErr = llvm::parseBitcodeFile(buf->getMemBufferRef(), ctx2);
    ASSERT_TRUE(!!modOrErr);

    auto* tickFunc = modOrErr.get()->getFunction(names.tick);
    ASSERT_NE(tickFunc, nullptr);

    bool hasPlaceholder = false;
    for (auto& BB : *tickFunc) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = call->getCalledFunction()) {
                    auto d = llvm::demangle(callee->getName().str());
                    if (d.find("pipeline_placeholder") != std::string::npos) hasPlaceholder = true;
                }
            }
        }
    }
    EXPECT_FALSE(hasPlaceholder) << "Post-codegen IR should NOT contain pipeline_placeholder";
}

// ============================================================
// Bug 2: serializeMetadata includes calledFunctions
// ============================================================

TEST_F(JITSpecializeFixture, Metadata_IncludesCalledFunctions) {
    ParallelConfig cfg;
    cfg.mode = topo::FeatureMode::Auto;
    auto json = IREmbed::serializeMetadata(symbols, cfg);
    ASSERT_FALSE(json.empty());

    auto doc = nlohmann::json::parse(json);
    ASSERT_TRUE(doc.contains("pipelines"));

    auto& pipelines = doc["pipelines"];
    ASSERT_EQ(pipelines.size(), 1u);

    auto& pipeline = pipelines[0];
    EXPECT_EQ(pipeline["name"], "sim::tick");
    ASSERT_TRUE(pipeline.contains("calledFunctions")) << "Metadata must include calledFunctions field";

    auto& cf = pipeline["calledFunctions"];
    ASSERT_EQ(cf.size(), 3u);

    // Verify all stage functions are listed
    std::set<std::string> funcs;
    for (const auto& f : cf)
        funcs.insert(f.get<std::string>());
    EXPECT_TRUE(funcs.count("sim::stage_a"));
    EXPECT_TRUE(funcs.count("sim::stage_b"));
    EXPECT_TRUE(funcs.count("sim::stage_c"));
}

// ============================================================
// Bug 3: PipelineCodeGenPass works on deserialized pre-codegen IR
// (This is the core of Step 7)
// ============================================================

TEST_F(JITSpecializeFixture, Step7_RegeneratePipelineFromSerializedIR) {
    // Serialize pre-codegen IR + metadata (before any passes)
    auto bitcode = IREmbed::serializePreCodegenIR(*module, symbols, mapping);
    ASSERT_FALSE(bitcode.empty());

    ParallelConfig parCfg;
    parCfg.mode = topo::FeatureMode::Auto;
    auto metaJson = IREmbed::serializeMetadata(symbols, parCfg);

    // --- Simulate JIT Step 7: deserialize and re-codegen ---

    // Load bitcode into a fresh context
    auto buf = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char*>(bitcode.data()), bitcode.size()), "", false);
    llvm::LLVMContext jitCtx;
    auto modOrErr = llvm::parseBitcodeFile(buf->getMemBufferRef(), jitCtx);
    ASSERT_TRUE(!!modOrErr);
    auto jitModule = std::move(modOrErr.get());

    // Parse metadata
    auto doc = nlohmann::json::parse(metaJson);
    auto& pipelineJson = doc["pipelines"][0];

    // 7a: Build SymbolMapping by demangling
    SymbolMapping jitMapping;
    for (auto& func : *jitModule) {
        auto demangled = llvm::demangle(func.getName().str());
        if (demangled == func.getName().str()) continue;
        auto parenPos = demangled.find('(');
        if (parenPos != std::string::npos) demangled = demangled.substr(0, parenPos);
        while (!demangled.empty() && demangled.back() == ' ')
            demangled.pop_back();
        auto lastSpace = demangled.rfind(' ');
        if (lastSpace != std::string::npos) {
            auto candidate = demangled.substr(lastSpace + 1);
            if (!candidate.empty() && (std::isalpha(static_cast<unsigned char>(candidate[0])) || candidate[0] == '_'))
                demangled = candidate;
        }
        jitMapping.matched[demangled] = &func;
    }

    // Verify mapping found all functions
    ASSERT_TRUE(jitMapping.matched.count("sim::tick")) << "Mapping must find sim::tick";
    ASSERT_TRUE(jitMapping.matched.count("sim::stage_a")) << "Mapping must find sim::stage_a";
    ASSERT_TRUE(jitMapping.matched.count("sim::stage_b")) << "Mapping must find sim::stage_b";
    ASSERT_TRUE(jitMapping.matched.count("sim::stage_c")) << "Mapping must find sim::stage_c";

    // 7b: Build synthetic SymbolTable from metadata
    SymbolTable syntheticSymbols;
    {
        LogicBlockEntry lb;
        lb.qualifiedName = pipelineJson["name"].get<std::string>();
        auto colonPos = lb.qualifiedName.rfind("::");
        lb.simpleName = (colonPos != std::string::npos) ? lb.qualifiedName.substr(colonPos + 2) : lb.qualifiedName;
        lb.isPipeline = true;
        for (const auto& cf : pipelineJson["calledFunctions"])
            lb.calledFunctions.push_back(cf.get<std::string>());
        for (const auto& e : pipelineJson["edges"]) {
            PipelineEdge edge;
            edge.source = e.value("source", "");
            edge.target = e.value("target", "");
            lb.edges.push_back(edge);
        }
        PipelineAnalysis analysis;
        for (auto& [node, stage] : pipelineJson["stages"].items())
            analysis.stages[node] = stage.get<int>();
        for (const auto& s : pipelineJson["sourceNodes"])
            analysis.sourceNodes.push_back(s.get<std::string>());
        analysis.terminalNode = pipelineJson["terminalNode"];
        analysis.terminalType = pipelineJson["terminalType"];
        for (auto& [node, fields] : pipelineJson["demand"].items()) {
            std::set<std::string> fs;
            for (const auto& f : fields)
                fs.insert(f.get<std::string>());
            analysis.demand[node] = fs;
        }
        lb.pipelineAnalysis = analysis;
        syntheticSymbols.addLogicBlock(lb);
    }

    // 7d: Run PipelineCodeGenPass
    int generated = PipelineCodeGenPass::run(*jitModule, syntheticSymbols, jitMapping);
    EXPECT_EQ(generated, 1) << "PipelineCodeGenPass should generate 1 pipeline body "
                               "from deserialized pre-codegen IR";

    // Verify: stub no longer has placeholder call
    auto* tickFunc = jitMapping.matched["sim::tick"];
    ASSERT_NE(tickFunc, nullptr);
    EXPECT_FALSE(tickFunc->isDeclaration());

    bool hasPlaceholder = false;
    bool callsStageA = false;
    bool callsStageB = false;
    bool callsStageC = false;
    for (auto& BB : *tickFunc) {
        for (auto& I : BB) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&I);
            if (!call || !call->getCalledFunction()) continue;
            auto calleeName = call->getCalledFunction()->getName().str();
            auto d = llvm::demangle(calleeName);
            if (d.find("pipeline_placeholder") != std::string::npos) hasPlaceholder = true;
            if (calleeName == names.stage_a) callsStageA = true;
            if (calleeName == names.stage_b) callsStageB = true;
            if (calleeName == names.stage_c) callsStageC = true;
        }
    }
    EXPECT_FALSE(hasPlaceholder) << "Generated body should not contain pipeline_placeholder";
    EXPECT_TRUE(callsStageA) << "Generated body should call stage_a";
    EXPECT_TRUE(callsStageB) << "Generated body should call stage_b";
    EXPECT_TRUE(callsStageC) << "Generated body should call stage_c";

    // Verify module is well-formed
    std::string err;
    llvm::raw_string_ostream errStream(err);
    EXPECT_FALSE(llvm::verifyModule(*jitModule, &errStream)) << "JIT module verification failed: " << err;
}

// ============================================================
// Edge pruning: removing edges makes nodes unreachable,
// and the generated pipeline should not call pruned stages.
// ============================================================

TEST_F(JITSpecializeFixture, Step7_PruneEdge_RemovesUnreachableNode) {
    // Serialize pre-codegen IR + metadata
    auto bitcode = IREmbed::serializePreCodegenIR(*module, symbols, mapping);
    ASSERT_FALSE(bitcode.empty());

    ParallelConfig parCfg;
    parCfg.mode = topo::FeatureMode::Off; // disable parallel for clarity
    auto metaJson = IREmbed::serializeMetadata(symbols, parCfg);
    auto doc = nlohmann::json::parse(metaJson);
    auto pipelineJson = doc["pipelines"][0];

    // Prune edge: stage_b -> stage_c
    // This makes stage_b unreachable from source nodes IF stage_b
    // is only reachable via its own source status.
    // Actually stage_b IS a source node, so it's always reachable.
    // Let's instead make a 4-node pipeline with a non-source node
    // that becomes unreachable.

    // Instead, modify the metadata to simulate pruning:
    // Remove stage_b from sourceNodes and remove the edge stage_b->stage_c
    // This makes stage_b have no incoming edges AND not a source node,
    // so it should be pruned.
    pipelineJson["sourceNodes"] = nlohmann::json::array({"stage_a"});
    // Keep only edge stage_a -> stage_c
    pipelineJson["edges"] = nlohmann::json::array({{{"source", "stage_a"}, {"target", "stage_c"}}});
    // stage_c now only needs stage_a's output

    // Load bitcode
    auto buf = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char*>(bitcode.data()), bitcode.size()), "", false);
    llvm::LLVMContext jitCtx;
    auto modOrErr = llvm::parseBitcodeFile(buf->getMemBufferRef(), jitCtx);
    ASSERT_TRUE(!!modOrErr);
    auto jitModule = std::move(modOrErr.get());

    // Build mapping
    SymbolMapping jitMapping;
    for (auto& func : *jitModule) {
        auto demangled = llvm::demangle(func.getName().str());
        if (demangled == func.getName().str()) continue;
        auto parenPos = demangled.find('(');
        if (parenPos != std::string::npos) demangled = demangled.substr(0, parenPos);
        while (!demangled.empty() && demangled.back() == ' ')
            demangled.pop_back();
        auto lastSpace = demangled.rfind(' ');
        if (lastSpace != std::string::npos) {
            auto candidate = demangled.substr(lastSpace + 1);
            if (!candidate.empty() && (std::isalpha(static_cast<unsigned char>(candidate[0])) || candidate[0] == '_'))
                demangled = candidate;
        }
        jitMapping.matched[demangled] = &func;
    }

    // Build synthetic SymbolTable with pruned DAG
    SymbolTable syntheticSymbols;
    {
        LogicBlockEntry lb;
        lb.qualifiedName = "sim::tick";
        lb.simpleName = "tick";
        lb.isPipeline = true;
        lb.calledFunctions = {"sim::stage_a", "sim::stage_b", "sim::stage_c"};

        // Pruned edges
        for (const auto& e : pipelineJson["edges"]) {
            PipelineEdge edge;
            edge.source = e.value("source", "");
            edge.target = e.value("target", "");
            lb.edges.push_back(edge);
        }

        PipelineAnalysis analysis;
        for (auto& [node, stage] : pipelineJson["stages"].items())
            analysis.stages[node] = stage.get<int>();
        analysis.sourceNodes = {"stage_a"};
        analysis.terminalNode = "stage_c";
        analysis.terminalType = "int";

        // BFS to find reachable nodes
        std::set<std::string> reachable(analysis.sourceNodes.begin(), analysis.sourceNodes.end());
        std::vector<std::string> worklist(analysis.sourceNodes.begin(), analysis.sourceNodes.end());
        while (!worklist.empty()) {
            auto cur = worklist.back();
            worklist.pop_back();
            for (const auto& edge : lb.edges) {
                if (edge.source == cur && reachable.find(edge.target) == reachable.end()) {
                    reachable.insert(edge.target);
                    worklist.push_back(edge.target);
                }
            }
        }
        // Remove unreachable from stages
        for (auto it = analysis.stages.begin(); it != analysis.stages.end();) {
            if (reachable.find(it->first) == reachable.end())
                it = analysis.stages.erase(it);
            else
                ++it;
        }

        lb.pipelineAnalysis = analysis;
        syntheticSymbols.addLogicBlock(lb);
    }

    // Run PipelineCodeGenPass
    int generated = PipelineCodeGenPass::run(*jitModule, syntheticSymbols, jitMapping);
    EXPECT_EQ(generated, 1);

    // Verify: stage_b is NOT called (pruned)
    auto* tickFunc = jitMapping.matched["sim::tick"];
    ASSERT_NE(tickFunc, nullptr);

    bool callsStageA = false;
    bool callsStageB = false;
    bool callsStageC = false;
    for (auto& BB : *tickFunc) {
        for (auto& I : BB) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&I);
            if (!call || !call->getCalledFunction()) continue;
            auto calleeName = call->getCalledFunction()->getName().str();
            if (calleeName == names.stage_a) callsStageA = true;
            if (calleeName == names.stage_b) callsStageB = true;
            if (calleeName == names.stage_c) callsStageC = true;
        }
    }
    EXPECT_TRUE(callsStageA) << "Reachable stage_a should still be called";
    EXPECT_FALSE(callsStageB) << "Unreachable stage_b should NOT be called after edge pruning";
    EXPECT_TRUE(callsStageC) << "Terminal stage_c should still be called";
}
