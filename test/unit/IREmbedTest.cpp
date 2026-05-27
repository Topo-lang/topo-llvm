#include "topo/Backend/IREmbed.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"
#include "topo/Transforms/TopoParallelPass.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

using namespace topo;

namespace {

// Helper: create a simple module with a pipeline function
struct EmbedTestFixture {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("embed_test", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "my_pipeline", *module);
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(func->getArg(0));

        LogicBlockEntry lb;
        lb.qualifiedName = "ns::my_pipeline";
        lb.simpleName = "my_pipeline";
        lb.isPipeline = true;
        lb.edges = {{"a", "b"}, {"b", "c"}};

        PipelineAnalysis analysis;
        analysis.stages = {{"a", 0}, {"b", 1}, {"c", 2}};
        analysis.sourceNodes = {"a"};
        analysis.terminalNode = "c";
        analysis.terminalType = "int";
        lb.pipelineAnalysis = analysis;

        symbols.addLogicBlock(lb);
        mapping.matched["ns::my_pipeline"] = func;
    }
};

TEST(IREmbedTest, SerializeBitcodeRoundTrip) {
    llvm::LLVMContext ctx;
    EmbedTestFixture fixture;
    fixture.build(ctx);

    auto bitcode = IREmbed::serializePipelineIR(*fixture.module, fixture.symbols, fixture.mapping);

    ASSERT_FALSE(bitcode.empty());

    // Verify we can parse it back
    auto buf = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char*>(bitcode.data()), bitcode.size()), "", false);

    llvm::LLVMContext ctx2;
    auto moduleOrErr = llvm::parseBitcodeFile(buf->getMemBufferRef(), ctx2);
    ASSERT_TRUE(!!moduleOrErr) << "Failed to parse bitcode";

    auto& restored = *moduleOrErr.get();
    auto* func = restored.getFunction("my_pipeline");
    ASSERT_NE(func, nullptr);
    EXPECT_FALSE(func->isDeclaration());
}

TEST(IREmbedTest, SerializeMetadataJSON) {
    llvm::LLVMContext ctx;
    EmbedTestFixture fixture;
    fixture.build(ctx);

    ParallelConfig parallelCfg;
    parallelCfg.mode = topo::FeatureMode::Auto;

    auto json = IREmbed::serializeMetadata(fixture.symbols, parallelCfg);
    ASSERT_FALSE(json.empty());

    auto doc = nlohmann::json::parse(json);
    ASSERT_TRUE(doc.contains("pipelines"));
    ASSERT_TRUE(doc.contains("parallel"));

    auto& pipelines = doc["pipelines"];
    ASSERT_EQ(pipelines.size(), 1u);
    EXPECT_EQ(pipelines[0]["name"], "ns::my_pipeline");

    auto& edges = pipelines[0]["edges"];
    EXPECT_EQ(edges.size(), 2u);

    auto& par = doc["parallel"];
    EXPECT_EQ(par["enabled"], true);
    // minTasksToParallelize removed — Topo passes do not
    // gate on workload-side cost heuristics; field no longer serialized.
    EXPECT_FALSE(par.contains("minTasksToParallelize"));
}

TEST(IREmbedTest, EmbedCreatesGlobalVariables) {
    llvm::LLVMContext ctx;
    EmbedTestFixture fixture;
    fixture.build(ctx);

    std::vector<uint8_t> bitcode = {0x42, 0x43, 0x00};
    std::string metadata = R"({"test": true})";

    IREmbed::embed(*fixture.module, bitcode, metadata);

    // Search for globals by name prefix (private linkage may add suffix)
    llvm::GlobalVariable* irGV = nullptr;
    llvm::GlobalVariable* metaGV = nullptr;
    for (auto& gv : fixture.module->globals()) {
        if (gv.getName().starts_with("topo_embedded_ir")) irGV = &gv;
        if (gv.getName().starts_with("topo_embedded_meta")) metaGV = &gv;
    }

    ASSERT_NE(irGV, nullptr) << "topo_embedded_ir global not found";
    ASSERT_NE(metaGV, nullptr) << "topo_embedded_meta global not found";

    EXPECT_TRUE(irGV->isConstant());
    EXPECT_TRUE(metaGV->isConstant());
}

} // namespace
