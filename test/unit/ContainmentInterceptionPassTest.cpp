#include "topo/Transforms/ContainmentInterceptionPass.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class ContainmentInterceptionPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// Build a module with one caller function and one restricted-API declaration.
// The caller makes a single call to the restricted API (e.g. `fopen`).
//
// `targetTriple` lets a test pick MachO (underscore-prefixed C symbols) or a
// plain triple; `restrictedApiName` is the IR symbol name of the restricted
// callee.
struct ContainmentFixture {
    std::unique_ptr<llvm::Module> module;
    llvm::Function* caller = nullptr;
    llvm::Function* restricted = nullptr;
    SymbolTable symbols;
    SymbolMapping mapping;

    void build(llvm::LLVMContext& ctx,
                const std::string& restrictedApiName = "fopen",
                const std::string& callerQualName = "ns::worker",
                bool callerIsExternal = false) {
        module = std::make_unique<llvm::Module>("containment_demo", ctx);
        auto* voidTy = llvm::Type::getVoidTy(ctx);
        auto* ptrTy = llvm::PointerType::getUnqual(ctx);

        // Restricted API: a declaration (no body), classified by the catalog.
        auto* apiTy = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
        restricted = llvm::Function::Create(
            apiTy, llvm::GlobalValue::ExternalLinkage, restrictedApiName, *module);

        // Caller: a non-external function that calls the restricted API.
        auto* callerTy = llvm::FunctionType::get(voidTy, {}, false);
        caller = llvm::Function::Create(
            callerTy, llvm::GlobalValue::ExternalLinkage, "worker", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", caller);
            llvm::IRBuilder<> b(bb);
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            b.CreateCall(restricted, {nullPtr, nullPtr});
            b.CreateRetVoid();
        }

        // SymbolTable: the caller is a declared Topo function.
        FunctionSymbol callerSym;
        callerSym.qualifiedName = callerQualName;
        callerSym.simpleName = callerQualName.substr(callerQualName.rfind("::") + 2);
        callerSym.isExternal = callerIsExternal;
        symbols.addFunction(callerSym);

        mapping.matched[callerQualName] = caller;
    }

    // Count __topo_containment_violation calls inserted into the caller body.
    int violationCallCount() const {
        int n = 0;
        for (auto& bb : *caller) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call) continue;
                auto* callee = call->getCalledFunction();
                if (callee && callee->getName() == "__topo_containment_violation") ++n;
            }
        }
        return n;
    }
};

// Core assertion: a non-external function calling a restricted API gets an
// interception call inserted. A no-op pass inserts nothing and fails this.
TEST_F(ContainmentInterceptionPassTest, NonExternalRestrictedCallIsIntercepted) {
    llvm::LLVMContext ctx;
    ContainmentFixture fx;
    fx.build(ctx, /*restrictedApiName=*/"fopen");

    int instrumented = ContainmentInterceptionPass::run(*fx.module, fx.symbols, fx.mapping);

    EXPECT_EQ(instrumented, 1) << "exactly one restricted call should be intercepted";
    EXPECT_EQ(fx.violationCallCount(), 1)
        << "a __topo_containment_violation call must be inserted before the restricted call";
    // The runtime hook must have been declared in the module.
    EXPECT_NE(fx.module->getFunction("__topo_containment_violation"), nullptr);
}

// The interception call must be inserted *before* the restricted call, so the
// violation is reported even if the restricted call aborts.
TEST_F(ContainmentInterceptionPassTest, ViolationCallPrecedesRestrictedCall) {
    llvm::LLVMContext ctx;
    ContainmentFixture fx;
    fx.build(ctx, /*restrictedApiName=*/"fopen");

    ASSERT_EQ(ContainmentInterceptionPass::run(*fx.module, fx.symbols, fx.mapping), 1);

    int violationIdx = -1, restrictedIdx = -1, idx = 0;
    for (auto& bb : *fx.caller) {
        for (auto& inst : bb) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                auto* callee = call->getCalledFunction();
                if (callee && callee->getName() == "__topo_containment_violation")
                    violationIdx = idx;
                else if (callee && callee->getName() == "fopen")
                    restrictedIdx = idx;
            }
            ++idx;
        }
    }
    ASSERT_GE(violationIdx, 0);
    ASSERT_GE(restrictedIdx, 0);
    EXPECT_LT(violationIdx, restrictedIdx)
        << "interception must be inserted before the restricted call";
}

// A function declared `external` in .topo is exempt: it is *expected* to reach
// restricted APIs, so no interception is inserted.
TEST_F(ContainmentInterceptionPassTest, ExternalDeclaredFunctionIsExempt) {
    llvm::LLVMContext ctx;
    ContainmentFixture fx;
    fx.build(ctx, /*restrictedApiName=*/"fopen", /*callerQualName=*/"ns::worker",
             /*callerIsExternal=*/true);

    int instrumented = ContainmentInterceptionPass::run(*fx.module, fx.symbols, fx.mapping);
    EXPECT_EQ(instrumented, 0) << "external-declared function must not be instrumented";
    EXPECT_EQ(fx.violationCallCount(), 0);
}

// A call to a safe (non-restricted) callee must not be intercepted.
TEST_F(ContainmentInterceptionPassTest, SafeCallNotIntercepted) {
    llvm::LLVMContext ctx;
    ContainmentFixture fx;
    // An ordinary in-program helper is not in the capability catalog →
    // not restricted. (Names like `memcmp`/`fopen`/`socket` *are* catalogued.)
    fx.build(ctx, /*restrictedApiName=*/"topo_pure_helper");

    int instrumented = ContainmentInterceptionPass::run(*fx.module, fx.symbols, fx.mapping);
    EXPECT_EQ(instrumented, 0) << "a non-restricted call must not be intercepted";
    EXPECT_EQ(fx.violationCallCount(), 0);
}

// The sidecar report receives one entry per intercepted call site, naming the
// caller (qualified) and the intercepted callee.
TEST_F(ContainmentInterceptionPassTest, ReportRecordsInterceptedCallSite) {
    llvm::LLVMContext ctx;
    ContainmentFixture fx;
    fx.build(ctx, /*restrictedApiName=*/"socket");

    backend::ContainmentInterceptionReport report;
    int instrumented = ContainmentInterceptionPass::run(*fx.module, fx.symbols, fx.mapping, &report);
    EXPECT_EQ(instrumented, 1);
    ASSERT_EQ(report.entries.size(), 1u);
    EXPECT_EQ(report.entries[0].callerFunction, "ns::worker");
    EXPECT_EQ(report.entries[0].interceptedCallee, "socket");
}

} // namespace
