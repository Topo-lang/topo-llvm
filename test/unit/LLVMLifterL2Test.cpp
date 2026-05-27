#include "topo/Decompile/LLVMLifter.h"
#include "topo/Transpile/TranspileModel.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

using namespace topo;
using namespace topo::decompile;
using namespace topo::transpile;

namespace {

class LLVMLifterL2Test : public ::testing::Test {
protected:
    llvm::LLVMContext ctx;

    void SetUp() override { llvm::InitializeNativeTarget(); }

    /// Count how many statements of a given kind appear in a body (non-recursive).
    static int countStmtKind(const std::vector<StmtPtr>& body, Stmt::Kind k) {
        int count = 0;
        for (const auto& s : body) {
            if (s && s->kind() == k) ++count;
        }
        return count;
    }

    /// Find the first statement of a given kind.
    static const Stmt* findFirst(const std::vector<StmtPtr>& body, Stmt::Kind k) {
        for (const auto& s : body) {
            if (s && s->kind() == k) return s.get();
        }
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// IfElseRecovery: conditional branch with then/else converging
// ---------------------------------------------------------------------------

TEST_F(LLVMLifterL2Test, IfElseRecovery) {
    // Build:
    //   define i32 @test_ifelse(i1 %cond) {
    //   entry:
    //     br i1 %cond, label %then, label %else
    //   then:
    //     br label %merge
    //   else:
    //     br label %merge
    //   merge:
    //     %result = phi i32 [1, %then], [2, %else]
    //     ret i32 %result
    //   }
    auto module = std::make_unique<llvm::Module>("test_ifelse", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i1Ty = llvm::Type::getInt1Ty(ctx);

    auto* funcTy = llvm::FunctionType::get(i32Ty, {i1Ty}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "test_ifelse", *module);
    func->getArg(0)->setName("cond");

    auto* entryBB = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* thenBB = llvm::BasicBlock::Create(ctx, "then", func);
    auto* elseBB = llvm::BasicBlock::Create(ctx, "else", func);
    auto* mergeBB = llvm::BasicBlock::Create(ctx, "merge", func);

    // entry: br i1 %cond, %then, %else
    llvm::IRBuilder<> b(entryBB);
    b.CreateCondBr(func->getArg(0), thenBB, elseBB);

    // then: store something, then branch to merge
    b.SetInsertPoint(thenBB);
    b.CreateBr(mergeBB);

    // else: store something different, then branch to merge
    b.SetInsertPoint(elseBB);
    b.CreateBr(mergeBB);

    // merge: phi + ret
    b.SetInsertPoint(mergeBB);
    auto* phi = b.CreatePHI(i32Ty, 2, "result");
    phi->addIncoming(llvm::ConstantInt::get(i32Ty, 1), thenBB);
    phi->addIncoming(llvm::ConstantInt::get(i32Ty, 2), elseBB);
    b.CreateRet(phi);

    ASSERT_FALSE(llvm::verifyFunction(*func, &llvm::errs()));

    // Lift at Structured level
    LLVMLifter lifter;
    SymbolTable metadata;
    auto result = lifter.liftFunctionStructured(*func, "test_ifelse", metadata);

    // Should produce an IfStmt with populated thenBody and elseBody
    const auto* ifStmt = static_cast<const IfStmt*>(findFirst(result.body, Stmt::Kind::If));
    ASSERT_NE(ifStmt, nullptr) << "Structured lift should produce an IfStmt";
    EXPECT_NE(ifStmt->condition, nullptr) << "IfStmt should have a condition";
    // The then branch leads to the merge with phi, so it may be empty in
    // terms of explicit statements (only the branch). The else branch similarly.
    // The key assertion is that the IfStmt itself is present with condition.
}

// ---------------------------------------------------------------------------
// IfElseRecoveryWithBodies: if/else with real work in both branches
// ---------------------------------------------------------------------------

TEST_F(LLVMLifterL2Test, IfElseRecoveryWithBodies) {
    // Build:
    //   define i32 @test_ifelse2(i1 %cond, i32 %x) {
    //   entry:
    //     br i1 %cond, label %then, label %else
    //   then:
    //     %a = add i32 %x, 10
    //     br label %merge
    //   else:
    //     %b = sub i32 %x, 5
    //     br label %merge
    //   merge:
    //     %result = phi i32 [%a, %then], [%b, %else]
    //     ret i32 %result
    //   }
    auto module = std::make_unique<llvm::Module>("test_ifelse2", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i1Ty = llvm::Type::getInt1Ty(ctx);

    auto* funcTy = llvm::FunctionType::get(i32Ty, {i1Ty, i32Ty}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "test_ifelse2", *module);
    func->getArg(0)->setName("cond");
    func->getArg(1)->setName("x");

    auto* entryBB = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* thenBB = llvm::BasicBlock::Create(ctx, "then", func);
    auto* elseBB = llvm::BasicBlock::Create(ctx, "else", func);
    auto* mergeBB = llvm::BasicBlock::Create(ctx, "merge", func);

    llvm::IRBuilder<> b(entryBB);
    b.CreateCondBr(func->getArg(0), thenBB, elseBB);

    b.SetInsertPoint(thenBB);
    auto* addVal = b.CreateAdd(func->getArg(1), llvm::ConstantInt::get(i32Ty, 10), "a");
    b.CreateBr(mergeBB);

    b.SetInsertPoint(elseBB);
    auto* subVal = b.CreateSub(func->getArg(1), llvm::ConstantInt::get(i32Ty, 5), "b");
    b.CreateBr(mergeBB);

    b.SetInsertPoint(mergeBB);
    auto* phi = b.CreatePHI(i32Ty, 2, "result");
    phi->addIncoming(addVal, thenBB);
    phi->addIncoming(subVal, elseBB);
    b.CreateRet(phi);

    ASSERT_FALSE(llvm::verifyFunction(*func, &llvm::errs()));

    LLVMLifter lifter;
    SymbolTable metadata;
    auto result = lifter.liftFunctionStructured(*func, "test_ifelse2", metadata);

    const auto* ifStmt = static_cast<const IfStmt*>(findFirst(result.body, Stmt::Kind::If));
    ASSERT_NE(ifStmt, nullptr) << "Structured lift should produce an IfStmt";
    EXPECT_FALSE(ifStmt->thenBody.empty()) << "IfStmt thenBody should contain the add instruction";
    EXPECT_FALSE(ifStmt->elseBody.empty()) << "IfStmt elseBody should contain the sub instruction";
}

// ---------------------------------------------------------------------------
// CountedLoopRecovery: canonical for-loop (phi + add + icmp + br)
// ---------------------------------------------------------------------------

TEST_F(LLVMLifterL2Test, CountedLoopRecovery) {
    // Build:
    //   define void @test_for(i32 %n) {
    //   entry:
    //     br label %header
    //   header:
    //     %iv = phi i32 [0, %entry], [%next, %latch]
    //     %cmp = icmp slt i32 %iv, %n
    //     br i1 %cmp, label %body, label %exit
    //   body:
    //     br label %latch
    //   latch:
    //     %next = add i32 %iv, 1
    //     br label %header
    //   exit:
    //     ret void
    //   }
    auto module = std::make_unique<llvm::Module>("test_for", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    auto* funcTy = llvm::FunctionType::get(voidTy, {i32Ty}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "test_for", *module);
    func->getArg(0)->setName("n");

    auto* entryBB = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* headerBB = llvm::BasicBlock::Create(ctx, "header", func);
    auto* bodyBB = llvm::BasicBlock::Create(ctx, "body", func);
    auto* latchBB = llvm::BasicBlock::Create(ctx, "latch", func);
    auto* exitBB = llvm::BasicBlock::Create(ctx, "exit", func);

    llvm::IRBuilder<> b(entryBB);
    b.CreateBr(headerBB);

    b.SetInsertPoint(headerBB);
    auto* iv = b.CreatePHI(i32Ty, 2, "iv");
    auto* cmp = b.CreateICmpSLT(iv, func->getArg(0), "cmp");
    b.CreateCondBr(cmp, bodyBB, exitBB);

    b.SetInsertPoint(bodyBB);
    b.CreateBr(latchBB);

    b.SetInsertPoint(latchBB);
    auto* next = b.CreateAdd(iv, llvm::ConstantInt::get(i32Ty, 1), "next");
    b.CreateBr(headerBB);

    iv->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entryBB);
    iv->addIncoming(next, latchBB);

    b.SetInsertPoint(exitBB);
    b.CreateRetVoid();

    ASSERT_FALSE(llvm::verifyFunction(*func, &llvm::errs()));

    LLVMLifter lifter;
    SymbolTable metadata;
    auto result = lifter.liftFunctionStructured(*func, "test_for", metadata);

    const auto* forStmt = static_cast<const ForStmt*>(findFirst(result.body, Stmt::Kind::For));
    ASSERT_NE(forStmt, nullptr) << "Counted loop should produce a ForStmt";
    EXPECT_NE(forStmt->init, nullptr) << "ForStmt should have init";
    EXPECT_NE(forStmt->condition, nullptr) << "ForStmt should have condition";
    EXPECT_NE(forStmt->increment, nullptr) << "ForStmt should have increment";
}

// ---------------------------------------------------------------------------
// WhileLoopRecovery: non-counted loop -> WhileStmt
// ---------------------------------------------------------------------------

TEST_F(LLVMLifterL2Test, WhileLoopRecovery) {
    // Build a loop with a non-canonical induction variable (no phi that
    // LLVM recognizes as canonical).
    //   define void @test_while(ptr %p) {
    //   entry:
    //     br label %header
    //   header:
    //     %val = load i32, ptr %p
    //     %cmp = icmp ne i32 %val, 0
    //     br i1 %cmp, label %body, label %exit
    //   body:
    //     br label %header
    //   exit:
    //     ret void
    //   }
    auto module = std::make_unique<llvm::Module>("test_while", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    auto* funcTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "test_while", *module);
    func->getArg(0)->setName("p");

    auto* entryBB = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* headerBB = llvm::BasicBlock::Create(ctx, "header", func);
    auto* bodyBB = llvm::BasicBlock::Create(ctx, "body", func);
    auto* exitBB = llvm::BasicBlock::Create(ctx, "exit", func);

    llvm::IRBuilder<> b(entryBB);
    b.CreateBr(headerBB);

    b.SetInsertPoint(headerBB);
    auto* val = b.CreateLoad(i32Ty, func->getArg(0), "val");
    auto* cmp = b.CreateICmpNE(val, llvm::ConstantInt::get(i32Ty, 0), "cmp");
    b.CreateCondBr(cmp, bodyBB, exitBB);

    b.SetInsertPoint(bodyBB);
    b.CreateBr(headerBB);

    b.SetInsertPoint(exitBB);
    b.CreateRetVoid();

    ASSERT_FALSE(llvm::verifyFunction(*func, &llvm::errs()));

    LLVMLifter lifter;
    SymbolTable metadata;
    auto result = lifter.liftFunctionStructured(*func, "test_while", metadata);

    const auto* whileStmt = static_cast<const WhileStmt*>(findFirst(result.body, Stmt::Kind::While));
    ASSERT_NE(whileStmt, nullptr) << "Non-counted loop should produce a WhileStmt";
    EXPECT_NE(whileStmt->condition, nullptr) << "WhileStmt should have a condition";
}

// ---------------------------------------------------------------------------
// DirectLevelUnchanged: same if/else IR lifted at Direct level -> IfStmt
// with empty bodies (no CFG reconstruction)
// ---------------------------------------------------------------------------

TEST_F(LLVMLifterL2Test, DirectLevelUnchanged) {
    // Same pattern as IfElseRecoveryWithBodies
    auto module = std::make_unique<llvm::Module>("test_direct", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i1Ty = llvm::Type::getInt1Ty(ctx);

    auto* funcTy = llvm::FunctionType::get(i32Ty, {i1Ty, i32Ty}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "test_direct", *module);
    func->getArg(0)->setName("cond");
    func->getArg(1)->setName("x");

    auto* entryBB = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* thenBB = llvm::BasicBlock::Create(ctx, "then", func);
    auto* elseBB = llvm::BasicBlock::Create(ctx, "else", func);
    auto* mergeBB = llvm::BasicBlock::Create(ctx, "merge", func);

    llvm::IRBuilder<> b(entryBB);
    b.CreateCondBr(func->getArg(0), thenBB, elseBB);

    b.SetInsertPoint(thenBB);
    auto* addVal = b.CreateAdd(func->getArg(1), llvm::ConstantInt::get(i32Ty, 10), "a");
    b.CreateBr(mergeBB);

    b.SetInsertPoint(elseBB);
    auto* subVal = b.CreateSub(func->getArg(1), llvm::ConstantInt::get(i32Ty, 5), "b_");
    b.CreateBr(mergeBB);

    b.SetInsertPoint(mergeBB);
    auto* phi = b.CreatePHI(i32Ty, 2, "result");
    phi->addIncoming(addVal, thenBB);
    phi->addIncoming(subVal, elseBB);
    b.CreateRet(phi);

    ASSERT_FALSE(llvm::verifyFunction(*func, &llvm::errs()));

    LLVMLifter lifter;
    SymbolTable metadata;
    // liftFunctionDirect takes a const ref
    const llvm::Function& constFunc = *func;
    auto result = lifter.liftFunctionDirect(constFunc, "test_direct", metadata);

    // At Direct level, IfStmt is produced but with empty bodies
    const auto* ifStmt = static_cast<const IfStmt*>(findFirst(result.body, Stmt::Kind::If));
    ASSERT_NE(ifStmt, nullptr) << "Direct level should still produce an IfStmt from the conditional branch";
    EXPECT_TRUE(ifStmt->thenBody.empty()) << "Direct level IfStmt should have empty thenBody (no CFG reconstruction)";
    EXPECT_TRUE(ifStmt->elseBody.empty()) << "Direct level IfStmt should have empty elseBody (no CFG reconstruction)";
}

// ---------------------------------------------------------------------------
// Itanium C++ exception-handling recovery
// ---------------------------------------------------------------------------

namespace {

// Recursively search a statement tree for the first TryCatchStmt.
const TryCatchStmt* findTryCatch(const std::vector<StmtPtr>& body) {
    for (const auto& s : body) {
        if (!s) continue;
        if (s->kind() == Stmt::Kind::TryCatch)
            return static_cast<const TryCatchStmt*>(s.get());
        if (s->kind() == Stmt::Kind::If) {
            const auto& i = static_cast<const IfStmt&>(*s);
            if (auto* t = findTryCatch(i.thenBody)) return t;
            if (auto* t = findTryCatch(i.elseBody)) return t;
        } else if (s->kind() == Stmt::Kind::For) {
            if (auto* t = findTryCatch(static_cast<const ForStmt&>(*s).body)) return t;
        } else if (s->kind() == Stmt::Kind::While) {
            if (auto* t = findTryCatch(static_cast<const WhileStmt&>(*s).body)) return t;
        }
    }
    return nullptr;
}

// Declare the Itanium C++ personality + the EH runtime helpers on a module.
struct ItaniumEHEnv {
    llvm::Function* personality;
    llvm::Function* beginCatch;
    llvm::Function* endCatch;
    llvm::Function* typeidFor;

    ItaniumEHEnv(llvm::Module& m, llvm::LLVMContext& ctx) {
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* ptr = llvm::PointerType::get(ctx, 0);
        personality = llvm::Function::Create(
            llvm::FunctionType::get(i32, true),
            llvm::GlobalValue::ExternalLinkage, "__gxx_personality_v0", m);
        beginCatch = llvm::Function::Create(
            llvm::FunctionType::get(ptr, {ptr}, false),
            llvm::GlobalValue::ExternalLinkage, "__cxa_begin_catch", m);
        endCatch = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false),
            llvm::GlobalValue::ExternalLinkage, "__cxa_end_catch", m);
        typeidFor = llvm::Function::Create(
            llvm::FunctionType::get(i32, {ptr}, false),
            llvm::GlobalValue::ExternalLinkage, "llvm.eh.typeid.for.p0", m);
    }
};

} // namespace

// (a) invoke + single typed catch + landingpad -> TryCatch with one
//     CatchClause carrying the demangled exception type.
TEST_F(LLVMLifterL2Test, EHSingleTypedCatchRecovery) {
    auto module = std::make_unique<llvm::Module>("test_eh_catch", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    ItaniumEHEnv eh(*module, ctx);

    // typeinfo for std::length_error
    auto* tiGlobal = new llvm::GlobalVariable(
        *module, ptrTy, /*isConstant=*/true,
        llvm::GlobalValue::ExternalLinkage, nullptr, "_ZTISt12length_error");

    // void @callee()  (the throwing call we will `invoke`)
    auto* callee = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, false),
        llvm::GlobalValue::ExternalLinkage, "callee", *module);

    auto* funcTy = llvm::FunctionType::get(voidTy, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage,
                                        "test_eh_catch", *module);
    func->setPersonalityFn(eh.personality);

    auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* cont = llvm::BasicBlock::Create(ctx, "cont", func);
    auto* lpad = llvm::BasicBlock::Create(ctx, "lpad", func);
    auto* catchBB = llvm::BasicBlock::Create(ctx, "catch", func);
    auto* ret = llvm::BasicBlock::Create(ctx, "ret", func);

    llvm::IRBuilder<> b(entry);
    b.CreateInvoke(callee, cont, lpad);

    // cont (normal path): just falls through to ret
    b.SetInsertPoint(cont);
    b.CreateBr(ret);

    // lpad: landingpad { ptr, i32 } catch ptr @_ZTISt12length_error
    b.SetInsertPoint(lpad);
    auto* lpTy = llvm::StructType::get(ptrTy, i32Ty);
    auto* lp = b.CreateLandingPad(lpTy, 1);
    lp->addClause(tiGlobal);
    auto* exnPtr = b.CreateExtractValue(lp, 0, "exn");
    auto* obj = b.CreateCall(eh.beginCatch, {exnPtr}, "obj");
    (void)obj;
    b.CreateBr(catchBB);

    // catch: call __cxa_end_catch, then continue to ret
    b.SetInsertPoint(catchBB);
    b.CreateCall(eh.endCatch, {});
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    b.CreateRetVoid();

    ASSERT_FALSE(llvm::verifyFunction(*func, &llvm::errs()));

    LLVMLifter lifter;
    SymbolTable metadata;
    auto result = lifter.liftFunctionStructured(*func, "test_eh_catch", metadata);

    const auto* tc = findTryCatch(result.body);
    ASSERT_NE(tc, nullptr) << "invoke+typed-catch should produce a TryCatchStmt";
    EXPECT_FALSE(tc->tryBody.empty()) << "tryBody should contain the invoked call";
    ASSERT_EQ(tc->catchClauses.size(), 1u) << "exactly one catch clause expected";
    const auto& cc = tc->catchClauses[0];
    ASSERT_FALSE(cc.exceptionType.nameParts.empty())
        << "catch clause should carry a recovered exception type";
    EXPECT_EQ(cc.exceptionType.nameParts[0], "std::length_error");
    EXPECT_FALSE(cc.body.empty()) << "catch body should be non-empty";
    EXPECT_TRUE(tc->finallyBody.empty()) << "no cleanup -> no finally";
}

// (b) invoke + cleanup-only landingpad + resume -> TryCatch with a
//     finallyBody and no bogus catch clause.
TEST_F(LLVMLifterL2Test, EHCleanupOnlyRecovery) {
    auto module = std::make_unique<llvm::Module>("test_eh_cleanup", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    ItaniumEHEnv eh(*module, ctx);

    auto* callee = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, false),
        llvm::GlobalValue::ExternalLinkage, "callee", *module);
    auto* dtor = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, false),
        llvm::GlobalValue::ExternalLinkage, "cleanup_dtor", *module);

    auto* func = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, false),
        llvm::GlobalValue::ExternalLinkage, "test_eh_cleanup", *module);
    func->setPersonalityFn(eh.personality);

    auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* cont = llvm::BasicBlock::Create(ctx, "cont", func);
    auto* lpad = llvm::BasicBlock::Create(ctx, "lpad", func);
    auto* ret = llvm::BasicBlock::Create(ctx, "ret", func);

    llvm::IRBuilder<> b(entry);
    b.CreateInvoke(callee, cont, lpad);

    b.SetInsertPoint(cont);
    b.CreateCall(dtor, {});
    b.CreateBr(ret);

    // lpad: cleanup landingpad; run dtor; resume (rethrow)
    b.SetInsertPoint(lpad);
    auto* lpTy = llvm::StructType::get(ptrTy, i32Ty);
    auto* lp = b.CreateLandingPad(lpTy, 0);
    lp->setCleanup(true);
    b.CreateCall(dtor, {});
    b.CreateResume(lp);

    b.SetInsertPoint(ret);
    b.CreateRetVoid();

    ASSERT_FALSE(llvm::verifyFunction(*func, &llvm::errs()));

    LLVMLifter lifter;
    SymbolTable metadata;
    auto result = lifter.liftFunctionStructured(*func, "test_eh_cleanup", metadata);

    const auto* tc = findTryCatch(result.body);
    ASSERT_NE(tc, nullptr) << "invoke+cleanup should produce a TryCatchStmt";
    EXPECT_TRUE(tc->catchClauses.empty())
        << "cleanup-only landingpad must not synthesize a catch clause";
    EXPECT_FALSE(tc->finallyBody.empty())
        << "cleanup path should be recovered as finallyBody";
}

// (c) regression guard: a function with NO EH lifts identically to before
//     (still an IfStmt with recovered then/else, no TryCatch anywhere).
TEST_F(LLVMLifterL2Test, EHAbsentNoRegression) {
    auto module = std::make_unique<llvm::Module>("test_no_eh", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i1Ty = llvm::Type::getInt1Ty(ctx);

    auto* funcTy = llvm::FunctionType::get(i32Ty, {i1Ty, i32Ty}, false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage,
                                        "test_no_eh", *module);
    func->getArg(0)->setName("cond");
    func->getArg(1)->setName("x");

    auto* entryBB = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* thenBB = llvm::BasicBlock::Create(ctx, "then", func);
    auto* elseBB = llvm::BasicBlock::Create(ctx, "else", func);
    auto* mergeBB = llvm::BasicBlock::Create(ctx, "merge", func);

    llvm::IRBuilder<> b(entryBB);
    b.CreateCondBr(func->getArg(0), thenBB, elseBB);

    b.SetInsertPoint(thenBB);
    auto* addVal = b.CreateAdd(func->getArg(1), llvm::ConstantInt::get(i32Ty, 10), "a");
    b.CreateBr(mergeBB);

    b.SetInsertPoint(elseBB);
    auto* subVal = b.CreateSub(func->getArg(1), llvm::ConstantInt::get(i32Ty, 5), "b_");
    b.CreateBr(mergeBB);

    b.SetInsertPoint(mergeBB);
    auto* phi = b.CreatePHI(i32Ty, 2, "result");
    phi->addIncoming(addVal, thenBB);
    phi->addIncoming(subVal, elseBB);
    b.CreateRet(phi);

    ASSERT_FALSE(llvm::verifyFunction(*func, &llvm::errs()));

    LLVMLifter lifter;
    SymbolTable metadata;
    auto result = lifter.liftFunctionStructured(*func, "test_no_eh", metadata);

    EXPECT_EQ(findTryCatch(result.body), nullptr)
        << "non-EH function must not grow a spurious TryCatch";
    const auto* ifStmt = static_cast<const IfStmt*>(findFirst(result.body, Stmt::Kind::If));
    ASSERT_NE(ifStmt, nullptr) << "if/else recovery must still work unchanged";
    EXPECT_FALSE(ifStmt->thenBody.empty());
    EXPECT_FALSE(ifStmt->elseBody.empty());
}



namespace {
/// Build an unmatched function `f(StructTy %0)` and lift it (no SymbolTable
/// entry → liftType() runs on the parameter type). Returns the recovered
/// parameter TypeNode.
TypeNode liftStructParamType(llvm::LLVMContext& ctx, llvm::StructType* st) {
    auto module = std::make_unique<llvm::Module>("tmpl_test", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* funcTy = llvm::FunctionType::get(voidTy, {st}, false);
    auto* func =
        llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "f", *module);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> b(bb);
    b.CreateRetVoid();

    LLVMLifter lifter;
    SymbolTable metadata; // empty → unmatched path → liftType on the param
    auto result = lifter.liftFunctionStructured(*func, "f", metadata);
    EXPECT_EQ(result.params.size(), 1u);
    return result.params.empty() ? TypeNode{} : result.params[0].type;
}
} // namespace

TEST_F(LLVMLifterL2Test, TemplateArgsRecoveredFromStructName) {
    // %"class.Wrapper<int>" → nameParts=["Wrapper"], templateArgs=[ {int} ].
    auto* st = llvm::StructType::create(ctx, {llvm::Type::getInt32Ty(ctx)},
                                        "class.Wrapper<int>");
    TypeNode t = liftStructParamType(ctx, st);

    ASSERT_EQ(t.nameParts.size(), 1u);
    EXPECT_EQ(t.nameParts[0], "Wrapper");
    ASSERT_EQ(t.templateArgs.size(), 1u);
    ASSERT_EQ(t.templateArgs[0].nameParts.size(), 1u);
    EXPECT_EQ(t.templateArgs[0].nameParts[0], "int");
    EXPECT_TRUE(t.templateArgs[0].templateArgs.empty());
}

TEST_F(LLVMLifterL2Test, TemplateArgsRecoveredMultiArgQualifiedAndNested) {
    // %"class.ns::Container<int, double>" → qualified base + two args.
    auto* st1 = llvm::StructType::create(
        ctx, {llvm::Type::getInt32Ty(ctx)}, "class.ns::Container<int, double>");
    TypeNode t1 = liftStructParamType(ctx, st1);
    ASSERT_EQ(t1.nameParts.size(), 2u);
    EXPECT_EQ(t1.nameParts[0], "ns");
    EXPECT_EQ(t1.nameParts[1], "Container");
    ASSERT_EQ(t1.templateArgs.size(), 2u);
    EXPECT_EQ(t1.templateArgs[0].nameParts[0], "int");
    EXPECT_EQ(t1.templateArgs[1].nameParts[0], "double");

    // Nested: %"class.Outer<Inner<int>>" → recursive templateArgs.
    auto* st2 = llvm::StructType::create(
        ctx, {llvm::Type::getInt32Ty(ctx)}, "class.Outer<Inner<int>>");
    TypeNode t2 = liftStructParamType(ctx, st2);
    ASSERT_EQ(t2.nameParts.size(), 1u);
    EXPECT_EQ(t2.nameParts[0], "Outer");
    ASSERT_EQ(t2.templateArgs.size(), 1u);
    EXPECT_EQ(t2.templateArgs[0].nameParts[0], "Inner");
    ASSERT_EQ(t2.templateArgs[0].templateArgs.size(), 1u);
    EXPECT_EQ(t2.templateArgs[0].templateArgs[0].nameParts[0], "int");

    // Non-type integer argument: %"struct.Buf<8>" → nonTypeValue == 8.
    auto* st3 = llvm::StructType::create(
        ctx, {llvm::Type::getInt32Ty(ctx)}, "struct.Buf<8>");
    TypeNode t3 = liftStructParamType(ctx, st3);
    ASSERT_EQ(t3.nameParts.size(), 1u);
    EXPECT_EQ(t3.nameParts[0], "Buf");
    ASSERT_EQ(t3.templateArgs.size(), 1u);
    ASSERT_TRUE(t3.templateArgs[0].nonTypeValue.has_value());
    EXPECT_EQ(*t3.templateArgs[0].nonTypeValue, 8);
}

TEST_F(LLVMLifterL2Test, NonTemplatedStructHasEmptyTemplateArgs) {
    // Regression guard: %"class.Plain" must stay exactly as before.
    auto* st = llvm::StructType::create(ctx, {llvm::Type::getInt32Ty(ctx)},
                                        "class.Plain");
    TypeNode t = liftStructParamType(ctx, st);
    ASSERT_EQ(t.nameParts.size(), 1u);
    EXPECT_EQ(t.nameParts[0], "Plain");
    EXPECT_TRUE(t.templateArgs.empty());
}

TEST_F(LLVMLifterL2Test, MalformedTemplateNameFallsBackConservatively) {
    // Unbalanced angle brackets must NOT crash and must NOT fabricate args;
    // the legacy single-nameParts behaviour is preserved verbatim.
    auto* st = llvm::StructType::create(ctx, {llvm::Type::getInt32Ty(ctx)},
                                        "class.Garb<le<int");
    TypeNode t = liftStructParamType(ctx, st);
    EXPECT_TRUE(t.templateArgs.empty());
    ASSERT_EQ(t.nameParts.size(), 1u);
    EXPECT_EQ(t.nameParts[0], "Garb<le<int");
}

} // namespace
