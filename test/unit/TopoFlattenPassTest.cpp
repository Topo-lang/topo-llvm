#include "topo/Transforms/TopoFlattenPass.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Basic/BuildTypes.h"
#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Passes/PassBuilder.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class TopoFlattenPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

static VisibilityEntry makeEntry(const std::string& name, Visibility vis) {
    VisibilityEntry e;
    e.qualifiedName = name;
    e.visibility = vis;
    return e;
}

// A trivial function with ExternalLinkage and an unreferenced body.
static llvm::Function* createUnreferencedFunc(llvm::Module& m, const std::string& name) {
    auto& ctx = m.getContext();
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* fty = llvm::FunctionType::get(i32Ty, {}, false);
    auto* f = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, name, m);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", f);
    llvm::IRBuilder<> b(bb);
    b.CreateRet(llvm::ConstantInt::get(i32Ty, 42));
    return f;
}

// Run LLVM's GlobalDCE — the standard pipeline pass TopoFlattenPass enables.
static void runGlobalDCE(llvm::Module& m) {
    llvm::PassBuilder pb;
    llvm::ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    llvm::ModulePassManager mpm;
    mpm.addPass(llvm::GlobalDCEPass());
    mpm.run(m, mam);
}

// A private function with no callers is demoted to InternalLinkage, and after
// GlobalDCE it is removed entirely. A no-op pass leaves it ExternalLinkage and
// GlobalDCE cannot touch it — so the function survives and the test fails.
TEST_F(TopoFlattenPassTest, PrivateDeadFunctionDemotedAndRemoved) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("flatten_dead", ctx);
    auto* dead = createUnreferencedFunc(*module, "dead_helper");

    SymbolMapping mapping;
    mapping.matched["ns::dead_helper"] = dead;
    std::vector<VisibilityEntry> entries = {makeEntry("ns::dead_helper", Visibility::Private)};

    int demoted = TopoFlattenPass::run(*module, entries, mapping, BuildMode::Dev);
    EXPECT_EQ(demoted, 1);
    ASSERT_NE(module->getFunction("dead_helper"), nullptr);
    EXPECT_TRUE(module->getFunction("dead_helper")->hasInternalLinkage())
        << "private dead function must be demoted to internal linkage";

    runGlobalDCE(*module);
    EXPECT_EQ(module->getFunction("dead_helper"), nullptr)
        << "demoted dead function must be removed by GlobalDCE";
}

// A public function must keep ExternalLinkage and survive GlobalDCE — it is a
// logical entry point even with no in-module callers.
TEST_F(TopoFlattenPassTest, PublicFunctionNotDemoted) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("flatten_public", ctx);
    auto* pub = createUnreferencedFunc(*module, "public_api");

    SymbolMapping mapping;
    mapping.matched["ns::public_api"] = pub;
    std::vector<VisibilityEntry> entries = {makeEntry("ns::public_api", Visibility::Public)};

    int demoted = TopoFlattenPass::run(*module, entries, mapping, BuildMode::Dev);
    EXPECT_EQ(demoted, 0);
    EXPECT_FALSE(pub->hasInternalLinkage());

    runGlobalDCE(*module);
    EXPECT_NE(module->getFunction("public_api"), nullptr)
        << "public function must survive GlobalDCE";
}

// Dev mode demotes private only; protected stays external.
TEST_F(TopoFlattenPassTest, ProtectedNotDemotedInDevMode) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("flatten_prot_dev", ctx);
    auto* prot = createUnreferencedFunc(*module, "prot_helper");

    SymbolMapping mapping;
    mapping.matched["ns::prot_helper"] = prot;
    std::vector<VisibilityEntry> entries = {makeEntry("ns::prot_helper", Visibility::Protected)};

    int demoted = TopoFlattenPass::run(*module, entries, mapping, BuildMode::Dev);
    EXPECT_EQ(demoted, 0);
    EXPECT_FALSE(prot->hasInternalLinkage());
}

// Aggressive mode also demotes protected (all TUs merged).
TEST_F(TopoFlattenPassTest, ProtectedDemotedInAggressiveMode) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("flatten_prot_aggr", ctx);
    auto* prot = createUnreferencedFunc(*module, "prot_helper");

    SymbolMapping mapping;
    mapping.matched["ns::prot_helper"] = prot;
    std::vector<VisibilityEntry> entries = {makeEntry("ns::prot_helper", Visibility::Protected)};

    int demoted = TopoFlattenPass::run(*module, entries, mapping, BuildMode::Aggressive);
    EXPECT_EQ(demoted, 1);
    EXPECT_TRUE(prot->hasInternalLinkage());
}

// The sidecar report names every demoted function.
TEST_F(TopoFlattenPassTest, ReportRecordsDemotedFunctions) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("flatten_report", ctx);
    auto* d1 = createUnreferencedFunc(*module, "dead_one");
    auto* d2 = createUnreferencedFunc(*module, "dead_two");

    SymbolMapping mapping;
    mapping.matched["ns::dead_one"] = d1;
    mapping.matched["ns::dead_two"] = d2;
    std::vector<VisibilityEntry> entries = {
        makeEntry("ns::dead_one", Visibility::Private),
        makeEntry("ns::dead_two", Visibility::Private),
    };

    backend::TopoFlattenReport report;
    int demoted = TopoFlattenPass::run(*module, entries, mapping, BuildMode::Dev, &report);
    EXPECT_EQ(demoted, 2);
    ASSERT_EQ(report.demotedFunctions.size(), 2u);
    std::vector<std::string> names = report.demotedFunctions;
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "ns::dead_one");
    EXPECT_EQ(names[1], "ns::dead_two");
}

} // namespace
