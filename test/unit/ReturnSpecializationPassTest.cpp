#include "topo/Transforms/ReturnSpecializationPass.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"
#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class ReturnSpecializationPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// Build a function with sret convention, storing `numFields` i32 values through
// GEPs off the sret parameter. Returns the function and the store instructions
// (one per field, indexed by field number).
struct SretMultiReturnFixture {
    std::unique_ptr<llvm::Module> module;
    llvm::Function* producer = nullptr;
    llvm::Function* consumer = nullptr;
    llvm::StructType* retSty = nullptr;
    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> entries;

    void build(llvm::LLVMContext& ctx, unsigned numFields, const std::vector<std::string>& usedNames) {
        module = std::make_unique<llvm::Module>("sret_demand", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* voidTy = llvm::Type::getVoidTy(ctx);

        std::vector<llvm::Type*> fieldTys(numFields, i32Ty);
        retSty = llvm::StructType::create(ctx, fieldTys, "struct.Result");

        std::vector<llvm::Type*> paramTys = {ptrTy};
        auto* producerTy = llvm::FunctionType::get(voidTy, paramTys, false);
        producer = llvm::Function::Create(producerTy, llvm::GlobalValue::InternalLinkage, "producer", *module);
        producer->addParamAttr(0, llvm::Attribute::getWithStructRetType(ctx, retSty));
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", producer);
            llvm::IRBuilder<> b(bb);
            auto* retPtr = producer->getArg(0);
            auto* zero = llvm::ConstantInt::get(i32Ty, 0);
            for (unsigned i = 0; i < numFields; ++i) {
                auto* gep = b.CreateGEP(retSty, retPtr, {zero, llvm::ConstantInt::get(i32Ty, i)});
                b.CreateStore(llvm::ConstantInt::get(i32Ty, static_cast<int>(i + 100)), gep);
            }
            b.CreateRetVoid();
        }

        // Consumer: calls producer. The callsite is recorded in SymbolTable
        // with usedReturns containing `usedNames`.
        auto* consumerTy = llvm::FunctionType::get(voidTy, {}, false);
        consumer = llvm::Function::Create(consumerTy, llvm::GlobalValue::ExternalLinkage, "consumer", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", consumer);
            llvm::IRBuilder<> b(bb);
            auto* alloca = b.CreateAlloca(retSty);
            b.CreateCall(producer, {alloca});
            b.CreateRetVoid();
        }

        // Symbol table setup.
        FunctionSymbol producerSym;
        producerSym.qualifiedName = "ns::producer";
        producerSym.simpleName = "producer";
        producerSym.visibility = Visibility::Private;
        producerSym.isMultiReturn = true;
        producerSym.returnParams.clear();
        for (unsigned i = 0; i < numFields; ++i) {
            ReturnParam rp;
            rp.name = "field" + std::to_string(i);
            producerSym.returnParams.push_back(rp);
        }
        symbols.addFunction(producerSym);

        CallSiteInfo cs;
        cs.caller = "ns::consumer";
        cs.callee = "ns::producer";
        for (const auto& n : usedNames) cs.usedReturns.insert(n);
        symbols.addCallSite(cs);

        mapping.matched["ns::producer"] = producer;
        mapping.matched["ns::consumer"] = consumer;

        VisibilityEntry producerVe;
        producerVe.qualifiedName = "ns::producer";
        producerVe.visibility = Visibility::Private;
        entries.push_back(producerVe);
        VisibilityEntry consumerVe;
        consumerVe.qualifiedName = "ns::consumer";
        consumerVe.visibility = Visibility::Public;
        entries.push_back(consumerVe);
    }

    // Inspect the producer's store value for the given field GEP chain.
    // Returns true if the stored value is UndefValue.
    bool storedValueIsUndef(unsigned fieldIdx) const {
        auto* i32Ty = llvm::Type::getInt32Ty(producer->getContext());
        for (auto& bb : *producer) {
            for (auto& inst : bb) {
                auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst);
                if (!store) continue;
                auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
                if (!gep || gep->getNumIndices() < 2) continue;
                auto* idx0 = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
                auto* idx1 = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2));
                if (!idx0 || idx0->getZExtValue() != 0) continue;
                if (!idx1 || idx1->getZExtValue() != fieldIdx) continue;
                (void)i32Ty;
                return llvm::isa<llvm::UndefValue>(store->getValueOperand());
            }
        }
        return false;
    }
};

TEST_F(ReturnSpecializationPassTest, NoSymbolTableIsConservative) {
    llvm::LLVMContext ctx;
    SretMultiReturnFixture fx;
    fx.build(ctx, /*numFields=*/3, {"field0"});

    int result = ReturnSpecializationPass::run(*fx.module, fx.entries, fx.mapping, /*symbols=*/nullptr);
    EXPECT_EQ(result, 0);

    // All fields still store non-undef values.
    for (unsigned i = 0; i < 3; ++i) EXPECT_FALSE(fx.storedValueIsUndef(i));
}

TEST_F(ReturnSpecializationPassTest, FullyUsedStructUnchanged) {
    llvm::LLVMContext ctx;
    SretMultiReturnFixture fx;
    fx.build(ctx, /*numFields=*/2, {"field0", "field1"});

    int result = ReturnSpecializationPass::run(*fx.module, fx.entries, fx.mapping, &fx.symbols);
    EXPECT_EQ(result, 0);
    EXPECT_FALSE(fx.storedValueIsUndef(0));
    EXPECT_FALSE(fx.storedValueIsUndef(1));
}

TEST_F(ReturnSpecializationPassTest, UnusedFieldReplacedWithUndef) {
    llvm::LLVMContext ctx;
    SretMultiReturnFixture fx;
    // Three fields, only field0 and field2 used. field1 is dead.
    fx.build(ctx, /*numFields=*/3, {"field0", "field2"});

    int result = ReturnSpecializationPass::run(*fx.module, fx.entries, fx.mapping, &fx.symbols);
    EXPECT_GE(result, 1);
    EXPECT_FALSE(fx.storedValueIsUndef(0));
    EXPECT_TRUE(fx.storedValueIsUndef(1));
    EXPECT_FALSE(fx.storedValueIsUndef(2));
}

TEST_F(ReturnSpecializationPassTest, SretConventionRewrite) {
    llvm::LLVMContext ctx;
    SretMultiReturnFixture fx;
    // Four fields, only the last one used.
    fx.build(ctx, /*numFields=*/4, {"field3"});

    int result = ReturnSpecializationPass::run(*fx.module, fx.entries, fx.mapping, &fx.symbols);
    EXPECT_GE(result, 3);
    EXPECT_TRUE(fx.storedValueIsUndef(0));
    EXPECT_TRUE(fx.storedValueIsUndef(1));
    EXPECT_TRUE(fx.storedValueIsUndef(2));
    EXPECT_FALSE(fx.storedValueIsUndef(3));
}

TEST_F(ReturnSpecializationPassTest, InsertvalueConventionRewrite) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("iv_demand", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    // Direct struct return with insertvalue chain. Use an i32 argument so
    // IRBuilder cannot constant-fold the insertvalue chain away.
    auto* retSty = llvm::StructType::create(ctx, {i32Ty, i32Ty, i32Ty}, "struct.IVResult");
    auto* producerTy = llvm::FunctionType::get(retSty, {i32Ty}, false);
    auto* producer = llvm::Function::Create(producerTy, llvm::GlobalValue::InternalLinkage, "producer_iv", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", producer);
        llvm::IRBuilder<> b(bb);
        auto* arg = producer->getArg(0);
        auto* f0 = b.CreateAdd(arg, llvm::ConstantInt::get(i32Ty, 10));
        auto* f1 = b.CreateAdd(arg, llvm::ConstantInt::get(i32Ty, 20));
        auto* f2 = b.CreateAdd(arg, llvm::ConstantInt::get(i32Ty, 30));
        llvm::Value* agg = llvm::UndefValue::get(retSty);
        agg = b.CreateInsertValue(agg, f0, {0});
        agg = b.CreateInsertValue(agg, f1, {1});
        agg = b.CreateInsertValue(agg, f2, {2});
        b.CreateRet(agg);
    }

    // Consumer that uses only field0 and field2.
    auto* consumerTy = llvm::FunctionType::get(voidTy, {}, false);
    auto* consumer = llvm::Function::Create(consumerTy, llvm::GlobalValue::ExternalLinkage, "consumer_iv", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", consumer);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(producer, {llvm::ConstantInt::get(i32Ty, 5)});
        b.CreateRetVoid();
    }

    SymbolTable symbols;
    FunctionSymbol producerSym;
    producerSym.qualifiedName = "ns::producer_iv";
    producerSym.simpleName = "producer_iv";
    producerSym.visibility = Visibility::Private;
    producerSym.isMultiReturn = true;
    producerSym.returnParams = {{{}, "field0", {}}, {{}, "field1", {}}, {{}, "field2", {}}};
    symbols.addFunction(producerSym);

    CallSiteInfo cs;
    cs.caller = "ns::consumer_iv";
    cs.callee = "ns::producer_iv";
    cs.usedReturns = {"field0", "field2"};
    symbols.addCallSite(cs);

    SymbolMapping mapping;
    mapping.matched["ns::producer_iv"] = producer;
    mapping.matched["ns::consumer_iv"] = consumer;

    std::vector<VisibilityEntry> entries;
    VisibilityEntry ivEntry;
    ivEntry.qualifiedName = "ns::producer_iv";
    ivEntry.visibility = Visibility::Private;
    entries.push_back(ivEntry);

    int result = ReturnSpecializationPass::run(*module, entries, mapping, &symbols);
    EXPECT_GE(result, 1);

    // Check that the insertvalue for field1 now inserts an undef value.
    bool sawUndefIv1 = false;
    bool sawNonUndefIv0 = false;
    bool sawNonUndefIv2 = false;
    for (auto& bb : *producer) {
        for (auto& inst : bb) {
            auto* iv = llvm::dyn_cast<llvm::InsertValueInst>(&inst);
            if (!iv || iv->getNumIndices() != 1) continue;
            unsigned idx = iv->getIndices()[0];
            auto* inserted = iv->getInsertedValueOperand();
            if (idx == 0 && !llvm::isa<llvm::UndefValue>(inserted)) sawNonUndefIv0 = true;
            if (idx == 1 && llvm::isa<llvm::UndefValue>(inserted)) sawUndefIv1 = true;
            if (idx == 2 && !llvm::isa<llvm::UndefValue>(inserted)) sawNonUndefIv2 = true;
        }
    }
    EXPECT_TRUE(sawNonUndefIv0);
    EXPECT_TRUE(sawUndefIv1);
    EXPECT_TRUE(sawNonUndefIv2);
}

TEST_F(ReturnSpecializationPassTest, ConflictingDemandInfoUnionLiveness) {
    llvm::LLVMContext ctx;
    SretMultiReturnFixture fx;
    fx.build(ctx, /*numFields=*/2, {"field0"});

    // Add a second call site that uses field1.
    CallSiteInfo extra;
    extra.caller = "ns::other_consumer";
    extra.callee = "ns::producer";
    extra.usedReturns = {"field1"};
    fx.symbols.addCallSite(extra);

    int result = ReturnSpecializationPass::run(*fx.module, fx.entries, fx.mapping, &fx.symbols);
    // Union of {field0} and {field1} = both live → no specialization.
    EXPECT_EQ(result, 0);
    EXPECT_FALSE(fx.storedValueIsUndef(0));
    EXPECT_FALSE(fx.storedValueIsUndef(1));
}

TEST_F(ReturnSpecializationPassTest, ExternalLinkageSkipped) {
    llvm::LLVMContext ctx;
    SretMultiReturnFixture fx;
    fx.build(ctx, /*numFields=*/3, {"field0"});

    // Flip the producer to external linkage — pass should skip it.
    fx.producer->setLinkage(llvm::GlobalValue::ExternalLinkage);

    int result = ReturnSpecializationPass::run(*fx.module, fx.entries, fx.mapping, &fx.symbols);
    EXPECT_EQ(result, 0);
    for (unsigned i = 0; i < 3; ++i) EXPECT_FALSE(fx.storedValueIsUndef(i));
}

// Declared `with returns(...)` ceiling drives elimination even with a
// single Full-style call site.  Without the ceiling, the pass would
// conservatively keep every field.
TEST_F(ReturnSpecializationPassTest, WithReturnsCeilingEliminatesDeadFields) {
    llvm::LLVMContext ctx;
    SretMultiReturnFixture fx;
    // Call site intentionally has no usedReturns (Full style fallback),
    // but the callee declares `with returns(field0, _, _)`.
    fx.build(ctx, /*numFields=*/3, /*usedNames=*/{});

    // Flip Full-style on the recorded call site.
    auto* producerSym = const_cast<FunctionSymbol*>(fx.symbols.findFunction("ns::producer"));
    ASSERT_NE(producerSym, nullptr);
    producerSym->hasUsedReturnsClause = true;
    producerSym->usedReturns.insert("field0");

    int result = ReturnSpecializationPass::run(*fx.module, fx.entries, fx.mapping, &fx.symbols);
    EXPECT_GE(result, 2) << "fields 1 and 2 should be eliminated per ceiling";
    EXPECT_FALSE(fx.storedValueIsUndef(0));
    EXPECT_TRUE(fx.storedValueIsUndef(1));
    EXPECT_TRUE(fx.storedValueIsUndef(2));
}

// Sidecar report receives one entry per touched function with kept /
// eliminated field indices. Functions that the pass skipped (conservative,
// all-live, no sret) must not appear.
TEST_F(ReturnSpecializationPassTest, ReportEntryPopulatedOnElimination) {
    llvm::LLVMContext ctx;
    SretMultiReturnFixture fx;
    fx.build(ctx, /*numFields=*/3, /*usedNames=*/{"field0"});

    backend::ReturnSpecializationReport report;
    int result = ReturnSpecializationPass::run(*fx.module, fx.entries, fx.mapping,
                                               &fx.symbols, &report);
    EXPECT_GE(result, 2);

    ASSERT_EQ(report.entries.size(), 1u);
    const auto& e = report.entries[0];
    EXPECT_EQ(e.hostFunction, "ns::producer");
    EXPECT_EQ(e.keptFieldIndices, (std::vector<int>{0}));
    EXPECT_EQ(e.eliminatedFieldIndices, (std::vector<int>{1, 2}));
}

// Null report pointer must be a no-op (back-compat for callers that don't
// care about detail).
TEST_F(ReturnSpecializationPassTest, NullReportIsNoop) {
    llvm::LLVMContext ctx;
    SretMultiReturnFixture fx;
    fx.build(ctx, /*numFields=*/3, /*usedNames=*/{"field0"});

    int result = ReturnSpecializationPass::run(*fx.module, fx.entries, fx.mapping,
                                               &fx.symbols, /*report=*/nullptr);
    EXPECT_GE(result, 2);
}

} // namespace
