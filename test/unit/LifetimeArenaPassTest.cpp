// Regression coverage for LifetimeArenaPass memory-safety fixes: the pass
// used to arena-convert allocations whose slot address escapes (and to keep
// stale analysis state across functions), producing use-after-free IR.
//
// These cases drive LifetimeArenaPass::run() over hand-built modules and
// assert on the rewritten IR. They focus on three correctness properties
// that were previously broken:
//
//   #6  A pointer whose alloca SLOT address leaks (the slot is passed by
//       address to a call, stored as a value, returned, …) must be treated
//       as escaping, so its allocation is NOT arena-converted — otherwise it
//       dangles after arena teardown.
//   #7  A declared end-of-lifetime call must drive arena teardown at that
//       point (not deferred to function return), and exactly once.
//   #8  With no declared end, a single block that both lands (landingpad)
//       and returns must get exactly one topo_arena_destroy — not two.

#include "topo/Transforms/LifetimeArenaPass.h"

#include "topo/Backend/SymbolMapper.h"
#include "topo/Build/PassConfig.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <gtest/gtest.h>

#include <string>

using namespace topo;

namespace {

// Count direct calls to a named function across the whole module.
int countCalls(llvm::Module& m, llvm::StringRef name) {
    int n = 0;
    for (auto& f : m)
        for (auto& bb : f)
            for (auto& inst : bb)
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
                    if (auto* callee = call->getCalledFunction())
                        if (callee->getName() == name) ++n;
    return n;
}

// Declare an external (no body) C function in the module.
llvm::Function* declExtern(llvm::Module& m, llvm::StringRef name, llvm::FunctionType* ty) {
    return llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, m);
}

// Shared fixture: a "frame" lifetime group with start=open, end=close, and a
// covered worker function `work` where the allocation lives. The owner logic
// block `run` calls open/work/close in stage order.
struct ArenaFixture {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;

    llvm::Function* runFunc = nullptr;   // owner
    llvm::Function* openFunc = nullptr;  // start of lifetime
    llvm::Function* closeFunc = nullptr; // end of lifetime
    llvm::Function* workFunc = nullptr;  // covered worker (holds the alloc)
    llvm::Function* mallocFunc = nullptr;
    llvm::Function* freeFunc = nullptr;
    llvm::Function* sinkFunc = nullptr; // opaque ptr consumer (escape source)

    llvm::LLVMContext* ctx = nullptr;
    llvm::Type* voidTy = nullptr;
    llvm::Type* i64Ty = nullptr;
    llvm::PointerType* ptrTy = nullptr;

    // Register the symbol-table scaffolding so analyzeLifetimes resolves a
    // scope spanning open..close (range mode), with `run` as the owner.
    void buildSymbols(bool withEnd) {
        LifetimeGroupEntry grp;
        grp.name = "frame";
        grp.startFunc = "open";
        grp.endFunc = withEnd ? "close" : "";
        grp.isOpenEnded = !withEnd; // open-ended when no explicit end
        grp.isSingleFunc = false;
        symbols.addLifetimeGroup(grp);

        auto addFn = [&](const std::string& qn, const std::string& sn) {
            FunctionSymbol fs;
            fs.qualifiedName = qn;
            fs.simpleName = sn;
            fs.visibility = Visibility::Protected;
            symbols.addFunction(fs);
        };
        addFn("app::open", "open");
        addFn("app::work", "work");
        addFn("app::close", "close");

        LogicBlockEntry lb;
        lb.qualifiedName = "app::run";
        lb.simpleName = "run";
        lb.calledFunctions = {"app::open", "app::work", "app::close"};
        lb.stages = {1, 2, 3}; // open < work < close
        symbols.addLogicBlock(lb);

        mapping.matched["app::run"] = runFunc;
        mapping.matched["app::open"] = openFunc;
        mapping.matched["app::work"] = workFunc;
        mapping.matched["app::close"] = closeFunc;
    }

    void declareRuntime() {
        openFunc = declExtern(*module, "open", llvm::FunctionType::get(voidTy, false));
        closeFunc = declExtern(*module, "close", llvm::FunctionType::get(voidTy, false));
        mallocFunc = declExtern(*module, "malloc", llvm::FunctionType::get(ptrTy, {i64Ty}, false));
        freeFunc = declExtern(*module, "free", llvm::FunctionType::get(voidTy, {ptrTy}, false));
        sinkFunc = declExtern(*module, "sink", llvm::FunctionType::get(voidTy, {ptrTy}, false));
    }

    void init(llvm::LLVMContext& c) {
        ctx = &c;
        module = std::make_unique<llvm::Module>("arena_fixture", c);
        voidTy = llvm::Type::getVoidTy(c);
        i64Ty = llvm::Type::getInt64Ty(c);
        ptrTy = llvm::PointerType::get(c, 0);
        declareRuntime();
    }
};

LifetimeConfig forceConfig() {
    LifetimeConfig cfg;
    cfg.mode = topo::FeatureMode::Force;
    return cfg;
}

// ---------------------------------------------------------------------------
// #7: declared end-of-lifetime call drives teardown at that point, once.
// ---------------------------------------------------------------------------
TEST(LifetimeArenaPassTest, EndCallDestroysAtEndPointExactlyOnce) {
    llvm::LLVMContext ctx;
    ArenaFixture fx;
    fx.init(ctx);

    // work(): trivially safe local alloc + free, body in the covered fn.
    fx.workFunc = llvm::Function::Create(
        llvm::FunctionType::get(fx.voidTy, false), llvm::GlobalValue::ExternalLinkage, "work", *fx.module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", fx.workFunc);
        llvm::IRBuilder<> b(bb);
        auto* p = b.CreateCall(fx.mallocFunc, {llvm::ConstantInt::get(fx.i64Ty, 32)});
        b.CreateCall(fx.freeFunc, {p});
        b.CreateRetVoid();
    }

    // run(): open(); work(); close(); ret  — single return block.
    fx.runFunc = llvm::Function::Create(
        llvm::FunctionType::get(fx.voidTy, false), llvm::GlobalValue::ExternalLinkage, "run", *fx.module);
    llvm::CallInst* closeCall = nullptr;
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", fx.runFunc);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(fx.openFunc, {});
        b.CreateCall(fx.workFunc, {});
        closeCall = b.CreateCall(fx.closeFunc, {});
        b.CreateRetVoid();
    }
    (void)closeCall;

    fx.buildSymbols(/*withEnd=*/true);

    int n = LifetimeArenaPass::run(*fx.module, fx.symbols, fx.mapping, forceConfig());
    ASSERT_EQ(n, 1) << "the trivially safe malloc should convert to arena_alloc";

    // Exactly one create and one destroy.
    EXPECT_EQ(countCalls(*fx.module, "topo_arena_create"), 1);
    EXPECT_EQ(countCalls(*fx.module, "topo_arena_destroy"), 1)
        << "end call present -> single teardown at the end point, none deferred to ret";

    // The destroy must be dominated by the close() call (teardown at the
    // declared end, not at function return). Locate close() then the destroy
    // within run() and assert close() precedes it in program order.
    bool sawClose = false, destroyAfterClose = false;
    for (auto& inst : llvm::instructions(*fx.runFunc)) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            auto* callee = call->getCalledFunction();
            if (callee && callee->getName() == "close") sawClose = true;
            if (callee && callee->getName() == "topo_arena_destroy" && sawClose) destroyAfterClose = true;
        }
    }
    EXPECT_TRUE(destroyAfterClose) << "arena_destroy must be injected after the discovered end call";

    EXPECT_FALSE(llvm::verifyModule(*fx.module, &llvm::errs())) << "rewritten IR must verify";
}

// ---------------------------------------------------------------------------
// #8: no declared end + a block that both lands and returns -> one destroy.
// ---------------------------------------------------------------------------
TEST(LifetimeArenaPassTest, LandingPadThatReturnsDestroysOnce) {
    llvm::LLVMContext ctx;
    ArenaFixture fx;
    fx.init(ctx);

    // work(): safe local alloc + free.
    fx.workFunc = llvm::Function::Create(
        llvm::FunctionType::get(fx.voidTy, false), llvm::GlobalValue::ExternalLinkage, "work", *fx.module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", fx.workFunc);
        llvm::IRBuilder<> b(bb);
        auto* p = b.CreateCall(fx.mallocFunc, {llvm::ConstantInt::get(fx.i64Ty, 32)});
        b.CreateCall(fx.freeFunc, {p});
        b.CreateRetVoid();
    }

    // close() exists as a function but is NOT in the group (no declared end),
    // so teardown falls back to exits. Build run() with a personality and a
    // landingpad cleanup block that ALSO ends in ret (the pathological shape
    // that previously produced two destroys in one block).
    fx.runFunc = llvm::Function::Create(
        llvm::FunctionType::get(fx.voidTy, false), llvm::GlobalValue::ExternalLinkage, "run", *fx.module);
    auto* personality = declExtern(
        *fx.module, "__gxx_personality_v0", llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx), true));
    fx.runFunc->setPersonalityFn(personality);

    auto* entry = llvm::BasicBlock::Create(ctx, "entry", fx.runFunc);
    auto* cont = llvm::BasicBlock::Create(ctx, "cont", fx.runFunc);
    auto* lpad = llvm::BasicBlock::Create(ctx, "lpad", fx.runFunc);
    {
        llvm::IRBuilder<> b(entry);
        b.CreateCall(fx.openFunc, {});
        // invoke work() so we have an unwind edge into the landing pad.
        b.CreateInvoke(fx.workFunc, cont, lpad, {});
    }
    {
        llvm::IRBuilder<> b(cont);
        b.CreateRetVoid();
    }
    {
        // landingpad cleanup block that immediately returns — single block
        // is BOTH a landing pad AND a return.
        llvm::IRBuilder<> b(lpad);
        auto* lpTy = llvm::StructType::get(fx.ptrTy, llvm::Type::getInt32Ty(ctx));
        auto* lp = b.CreateLandingPad(lpTy, 0);
        lp->setCleanup(true);
        b.CreateRetVoid();
    }

    fx.buildSymbols(/*withEnd=*/false);

    int n = LifetimeArenaPass::run(*fx.module, fx.symbols, fx.mapping, forceConfig());
    ASSERT_EQ(n, 1);

    // Teardown is injected at cont's ret and at lpad. lpad both lands and
    // returns; the fix must dedup it to ONE destroy in that block (not two).
    // Across the two exit blocks (cont, lpad) we expect exactly 2 destroys —
    // not 3 (which is what the un-deduped landingpad+ret double-push gave).
    EXPECT_EQ(countCalls(*fx.module, "topo_arena_destroy"), 2)
        << "lpad block that both lands and returns must contribute one destroy, not two";

    EXPECT_FALSE(llvm::verifyModule(*fx.module, &llvm::errs())) << "rewritten IR must verify";
}

// ---------------------------------------------------------------------------
// #6: pointer whose alloca slot address leaks -> escaping -> NOT converted.
// ---------------------------------------------------------------------------
TEST(LifetimeArenaPassTest, SpilledAllocaSlotAddressLeakDoesNotConvert) {
    llvm::LLVMContext ctx;
    ArenaFixture fx;
    fx.init(ctx);

    // work():
    //   slot = alloca ptr
    //   p = malloc(32)
    //   store p, slot
    //   sink(slot)     ; <-- leaks the SLOT ADDRESS (escape)
    //   q = load slot
    //   free(q)
    // The pointer round-trips through `slot`, whose address is then handed to
    // an opaque call. The pointer can outlive the arena -> must NOT convert.
    fx.workFunc = llvm::Function::Create(
        llvm::FunctionType::get(fx.voidTy, false), llvm::GlobalValue::ExternalLinkage, "work", *fx.module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", fx.workFunc);
        llvm::IRBuilder<> b(bb);
        auto* slot = b.CreateAlloca(fx.ptrTy);
        auto* p = b.CreateCall(fx.mallocFunc, {llvm::ConstantInt::get(fx.i64Ty, 32)});
        b.CreateStore(p, slot);
        b.CreateCall(fx.sinkFunc, {slot}); // slot ADDRESS escapes
        auto* q = b.CreateLoad(fx.ptrTy, slot);
        b.CreateCall(fx.freeFunc, {q});
        b.CreateRetVoid();
    }

    fx.runFunc = llvm::Function::Create(
        llvm::FunctionType::get(fx.voidTy, false), llvm::GlobalValue::ExternalLinkage, "run", *fx.module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", fx.runFunc);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(fx.openFunc, {});
        b.CreateCall(fx.workFunc, {});
        b.CreateCall(fx.closeFunc, {});
        b.CreateRetVoid();
    }

    fx.buildSymbols(/*withEnd=*/true);

    int n = LifetimeArenaPass::run(*fx.module, fx.symbols, fx.mapping, forceConfig());
    EXPECT_EQ(n, 0) << "slot-address leak escapes -> allocation must stay heap, free untouched";

    // The original malloc/free must survive intact; no arena_alloc emitted.
    EXPECT_EQ(countCalls(*fx.module, "malloc"), 1);
    EXPECT_EQ(countCalls(*fx.module, "free"), 1) << "the genuine free must not be removed";
    EXPECT_EQ(countCalls(*fx.module, "topo_arena_alloc"), 0);
}

} // namespace
