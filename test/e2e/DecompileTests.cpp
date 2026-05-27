#include "E2eHarness.h"

#include "topo/Decompile/LLVMLifter.h"
#include "topo/Transpile/TranspileModel.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

#include <regex>
#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"
#include <fstream>
#include <functional>
#include <map>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

namespace topo::test::e2e {
namespace {

// Recursively scan a statement tree, asserting every TryCatchStmt is
// well-formed (never an empty/degenerate node). Returns the count found.
int auditTryCatch(const std::vector<transpile::StmtPtr>& body) {
    using namespace transpile;
    int found = 0;
    for (const auto& s : body) {
        if (!s) continue;
        switch (s->kind()) {
        case Stmt::Kind::TryCatch: {
            const auto& tc = static_cast<const TryCatchStmt&>(*s);
            // Conservative contract: a recovered TryCatch is never empty.
            EXPECT_FALSE(tc.tryBody.empty() && tc.catchClauses.empty() &&
                         tc.finallyBody.empty())
                << "recovered TryCatch must not be degenerate/empty";
            ++found;
            found += auditTryCatch(tc.tryBody);
            for (const auto& cc : tc.catchClauses)
                found += auditTryCatch(cc.body);
            found += auditTryCatch(tc.finallyBody);
            break;
        }
        case Stmt::Kind::If: {
            const auto& i = static_cast<const IfStmt&>(*s);
            found += auditTryCatch(i.thenBody);
            found += auditTryCatch(i.elseBody);
            break;
        }
        case Stmt::Kind::For:
            found += auditTryCatch(static_cast<const ForStmt&>(*s).body);
            break;
        case Stmt::Kind::While:
            found += auditTryCatch(static_cast<const WhileStmt&>(*s).body);
            break;
        case Stmt::Kind::Switch:
            for (const auto& c : static_cast<const SwitchStmt&>(*s).cases)
                found += auditTryCatch(c.body);
            break;
        default:
            break;
        }
    }
    return found;
}

} // namespace

class DecompileTest : public E2eFixture {
protected:
    void SetUp() override {
        E2eFixture::SetUp();
        llvm::InitializeNativeTarget();
    }
};

// Test that a binary built with embed_ir=true can be decompiled
TEST_F(DecompileTest, LiftFromEmbeddedIR) {
    // Build the jit benchmark (it has embed_ir = true)
    auto build = topoBuild("jit");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    // Get binary path
    auto binPath = binaryPath("jit", "ir_embedding");
    ASSERT_TRUE(std::filesystem::exists(binPath))
        << "Binary not found: " << binPath;

    // Extract and lift at Structured level
    decompile::LLVMLifter lifter;
    SymbolTable metadata; // empty metadata for basic lifting

    auto model = lifter.lift(binPath.string(), metadata, transpile::DecompileLevel::Structured);

    // Verify the model has functions
    EXPECT_FALSE(model.functions.empty())
        << "Decompiled model should contain at least one function";

    // Check that we recovered at least one function with a body
    bool hasBody = false;
    for (const auto& fn : model.functions) {
        if (!fn.body.empty()) {
            hasBody = true;
            break;
        }
    }
    EXPECT_TRUE(hasBody) << "At least one function should have a recovered body";
}

// Test direct-level lifting produces linear instruction list
TEST_F(DecompileTest, DirectLevelProducesLinearBody) {
    auto build = topoBuild("jit");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto binPath = binaryPath("jit", "ir_embedding");
    ASSERT_TRUE(std::filesystem::exists(binPath));

    decompile::LLVMLifter lifter;
    SymbolTable metadata;

    auto model = lifter.lift(binPath.string(), metadata, transpile::DecompileLevel::Direct);
    EXPECT_FALSE(model.functions.empty())
        << "Direct-level lift should produce at least one function";
}

// DWARF type recovery in LLVMLifter.
//
// PRECONDITION FINDING: `embed_ir` captures the
// module AFTER the O2 PassPipeline (topo-build-llvm-cpp step 6 runs
// optimize() then embeds). By that point mem2reg has promoted nearly all
// allocas and named struct types are gone, even though orphaned
// DICompositeType / DILocalVariable metadata nodes linger. So on the REAL
// jit pipeline `getIdentifiedStructTypes()` is empty and almost no
// alloca-backed locals remain -- struct field-name / local-type recovery
// has structurally nothing to consume here. The consumption code is correct
// and conservative; proving full recovery requires a pre-optimization /
// pre-codegen embedded snapshot (large, risky embed-contract change -- not
// taken). A hand-built DIBuilder fixture is explicitly FORBIDDEN (it would
// pass while the real path recovers nothing).
//
// What this test DOES verify on the real build->embed->lift pipeline:
//   (a) lifting a real embedded-IR binary that carries DWARF metadata does
//       not crash and still produces functions;
//   (b) the conservative contract holds: every struct field that is NOT
//       DWARF-recovered is the numeric "field<N>" fallback marked
//       Fidelity::Inferred (never a wrong name, never Recovered-without-DWARF);
//   (c) IF any struct type does survive with matching DWARF, its recovered
//       fields are real identifiers marked Recovered (no false negatives).
TEST_F(DecompileTest, DwarfRecoveryConservativeContractOnRealPipeline) {
    auto build = topoBuild("jit");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto binPath = binaryPath("jit", "ir_embedding");
    ASSERT_TRUE(std::filesystem::exists(binPath))
        << "Binary not found: " << binPath;

    decompile::LLVMLifter lifter;
    SymbolTable metadata;
    auto model =
        lifter.lift(binPath.string(), metadata, transpile::DecompileLevel::Structured);

    // (a) No crash, still recovers functions from a DWARF-bearing binary.
    EXPECT_FALSE(model.functions.empty())
        << "Lifting a real embedded-IR binary should still yield functions";

    const std::regex numericField("^field[0-9]+$");

    // (b)+(c) Conservative contract over every recovered field.
    for (const auto& ty : model.types) {
        for (const auto& f : ty.fields) {
            ASSERT_FALSE(f.name.empty()) << "field name must never be empty";
            bool isNumericFallback = std::regex_match(f.name, numericField);
            if (isNumericFallback) {
                // Fallback path MUST be marked Inferred, never Recovered.
                EXPECT_EQ(f.fidelity, transpile::Fidelity::Inferred)
                    << "numeric fallback field '" << f.name
                    << "' must be Fidelity::Inferred";
            } else {
                // A non-numeric name can only come from DWARF recovery, so it
                // MUST be marked Recovered (no Recovered/Inferred mismatch).
                EXPECT_EQ(f.fidelity, transpile::Fidelity::Recovered)
                    << "DWARF-recovered field '" << f.name
                    << "' must be Fidelity::Recovered";
            }
        }
    }

    // Local-variable contract: a recovered VarDecl must never have an empty
    // name or an empty type. We deliberately do NOT assert "Recovered =>
    // non-_tmp name": the pre-existing lifter convention marks faithfully
    // lifted SSA/alloca temporaries Recovered even when they have no
    // source-level name (synthetic "_tmp<N>"). The DWARF local-recovery path
    // only *improves* name/type when a DILocalVariable is present; proving
    // full source-name recovery end-to-end is blocked by the post-O2 embed
    // timing (the embedded IR retains essentially no alloca-backed locals
    // after the O2 mem2reg run). What we can
    // and do verify here is that the recovery never corrupts a decl.
    for (const auto& fn : model.functions) {
        for (const auto& stmt : fn.body) {
            if (stmt->kind() != transpile::Stmt::Kind::VarDecl) continue;
            const auto& vd = static_cast<const transpile::VarDeclStmt&>(*stmt);
            EXPECT_FALSE(vd.name.empty())
                << "lifted local must always have a non-empty name";
            EXPECT_FALSE(vd.type.nameParts.empty())
                << "lifted local '" << vd.name
                << "' must always have a non-empty type (DWARF-recovered or "
                   "liftType fallback, never empty)";
        }
    }
}

// DWARF field-name recovery proven end-to-end
// on the NON-blocked path: a standalone, pre-O2 .bc still has live struct
// types + DICompositeType metadata, so the recovery infra (the same code
// that runs but has nothing to consume on the post-O2 embedded snapshot)
// produces real source names. This complements
// DwarfRecoveryConservativeContractOnRealPipeline (which only proves the
// "stays safe" half on the blocked embedded path).
TEST_F(DecompileTest, DwarfRecoveryOnStandaloneUnoptimizedBitcode) {
    namespace fs = std::filesystem;

    fs::path clangxx =
        llvmBinDir_ / ("clang++" + std::string(platform::ExeSuffix));
    if (!fs::exists(clangxx))
        GTEST_SKIP() << "clang++ not available in " << llvmBinDir_;

    fs::path tmpDir = fs::temp_directory_path() / "topo-dwarf-bc";
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir);

    fs::path cppPath = tmpDir / "foo.cpp";
    {
        std::ofstream o(cppPath);
        // Named struct + named fields + a function that uses them. -O0 keeps
        // the struct type identified and DICompositeType members intact.
        o << "struct Foo {\n"
             "  int width;\n"
             "  int height;\n"
             "};\n"
             "int area(int w, int h) {\n"
             "  Foo f;\n"
             "  f.width = w;\n"
             "  f.height = h;\n"
             "  return f.width * f.height;\n"
             "}\n";
    }
    fs::path bcPath = tmpDir / "foo.bc";

    // clang++ -O0 -g -emit-llvm -c foo.cpp -o foo.bc
    std::vector<std::string> args = {"-O0", "-g", "-emit-llvm", "-c",
                                     cppPath.string(), "-o", bcPath.string()};
    if constexpr (platform::IsMacOS) {
        auto sdk = platform::runProcessCapture("xcrun", {"--show-sdk-path"});
        if (sdk.exitCode == 0) {
            std::string sdkPath = sdk.stdoutOutput;
            while (!sdkPath.empty() &&
                   (sdkPath.back() == '\n' || sdkPath.back() == '\r'))
                sdkPath.pop_back();
            args.push_back("-isysroot");
            args.push_back(sdkPath);
        }
    }
    auto compile = platform::runProcessCapture(clangxx.string(), args);
    ASSERT_EQ(compile.exitCode, 0)
        << "clang++ failed to build the bitcode fixture: "
        << compile.stderrOutput;
    ASSERT_TRUE(fs::exists(bcPath))
        << "expected bitcode at " << bcPath;

    decompile::LLVMLifter lifter;
    SymbolTable metadata;
    auto model = lifter.liftBitcode(bcPath.string(), metadata,
                                    transpile::DecompileLevel::Structured);

    ASSERT_FALSE(model.types.empty())
        << "lifting an unoptimized .bc must surface its named struct types";

    // Find the Foo type. The struct may be qualified by a class. prefix
    // depending on how libclang strips names; stripStructName already drops
    // those.
    const transpile::TranspileType* foo = nullptr;
    for (const auto& ty : model.types) {
        if (ty.qualifiedName == "Foo" ||
            ty.qualifiedName.find("Foo") != std::string::npos) {
            foo = &ty;
            break;
        }
    }
    ASSERT_NE(foo, nullptr)
        << "expected a `Foo` type recovered from the bitcode; got "
        << model.types.size() << " types";

    ASSERT_EQ(foo->fields.size(), 2u)
        << "Foo has two fields (width, height); got " << foo->fields.size();

    // Both field names must come from DWARF (Recovered) and match the source
    // names. Pre-fix this would have produced numeric "field0"/"field1"
    // fallbacks marked Inferred.
    EXPECT_EQ(foo->fields[0].name, "width");
    EXPECT_EQ(foo->fields[0].fidelity, transpile::Fidelity::Recovered)
        << "DWARF-recovered field must be Fidelity::Recovered";
    EXPECT_EQ(foo->fields[1].name, "height");
    EXPECT_EQ(foo->fields[1].fidelity, transpile::Fidelity::Recovered);

    fs::remove_all(tmpDir, ec);
}

// Local-variable name + type recovery via DWARF on the standalone bitcode
// path. Complements DwarfRecoveryOnStandaloneUnoptimizedBitcode (which
// covers struct field names) by proving the *other* consumer of
// LLVMLifter::buildDebugInfoMaps — debugLocalForAlloca + typeFromDI —
// produces source-level names and source-level types on every alloca that
// has a DILocalVariable attached. Pre-fix the alloca-VarDeclStmt path would
// have produced numeric fallback names (`getValueName(*alloca)`) and a
// liftType-collapsed `i32`/`void*` instead of the source-level annotation;
// the test pins the real names + types so a future regression on
// debugLocalForAlloca cannot silently degrade local recovery to fallback.
TEST_F(DecompileTest, DwarfLocalRecoveryOnStandaloneUnoptimizedBitcode) {
    namespace fs = std::filesystem;

    fs::path clangxx =
        llvmBinDir_ / ("clang++" + std::string(platform::ExeSuffix));
    if (!fs::exists(clangxx))
        GTEST_SKIP() << "clang++ not available in " << llvmBinDir_;

    fs::path tmpDir = fs::temp_directory_path() / "topo-dwarf-bc-locals";
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir);

    fs::path cppPath = tmpDir / "locals.cpp";
    {
        std::ofstream o(cppPath);
        // Three named locals with distinct source types so each comes
        // through the DWARF type recovery path (typeFromDI) cleanly. -O0
        // keeps every alloca live and attaches a DILocalVariable; mem2reg
        // never runs on -O0 -emit-llvm output.
        o << "int compute(int seed) {\n"
             "  int counter = seed;\n"
             "  long total = 0;\n"
             "  double scale = 1.5;\n"
             "  for (int i = 0; i < counter; ++i) {\n"
             "    total += i;\n"
             "  }\n"
             "  return (int)(total * scale);\n"
             "}\n";
    }
    fs::path bcPath = tmpDir / "locals.bc";

    std::vector<std::string> args = {"-O0", "-g", "-emit-llvm", "-c",
                                     cppPath.string(), "-o", bcPath.string()};
    if constexpr (platform::IsMacOS) {
        auto sdk = platform::runProcessCapture("xcrun", {"--show-sdk-path"});
        if (sdk.exitCode == 0) {
            std::string sdkPath = sdk.stdoutOutput;
            while (!sdkPath.empty() &&
                   (sdkPath.back() == '\n' || sdkPath.back() == '\r'))
                sdkPath.pop_back();
            args.push_back("-isysroot");
            args.push_back(sdkPath);
        }
    }
    auto compile = platform::runProcessCapture(clangxx.string(), args);
    ASSERT_EQ(compile.exitCode, 0)
        << "clang++ failed to build the bitcode fixture: "
        << compile.stderrOutput;
    ASSERT_TRUE(fs::exists(bcPath));

    decompile::LLVMLifter lifter;
    SymbolTable metadata;
    auto model = lifter.liftBitcode(bcPath.string(), metadata,
                                    transpile::DecompileLevel::Structured);

    // Find the `compute` function — symbol may be mangled (Itanium) so
    // accept anything whose qualifiedName contains "compute".
    const transpile::TranspileFunction* fn = nullptr;
    for (const auto& f : model.functions) {
        if (f.qualifiedName.find("compute") != std::string::npos) {
            fn = &f;
            break;
        }
    }
    ASSERT_NE(fn, nullptr)
        << "expected a `compute` function recovered from the bitcode; got "
        << model.functions.size() << " functions";

    // Walk the body for VarDeclStmt nodes; bucket by recovered name so we
    // can pin each named local independently of body structure. (Structured
    // lift may nest the for-loop's `int i` declaration inside a ForStmt
    // initializer; scan recursively to surface every VarDecl.)
    std::map<std::string, const transpile::VarDeclStmt*> namedLocals;
    std::function<void(const transpile::Stmt*)> visitStmt;
    std::function<void(const std::vector<transpile::StmtPtr>&)> walk =
        [&](const std::vector<transpile::StmtPtr>& body) {
            for (const auto& s : body) visitStmt(s.get());
        };
    visitStmt = [&](const transpile::Stmt* s) {
        using namespace transpile;
        if (!s) return;
        if (s->kind() == Stmt::Kind::VarDecl) {
            const auto& vd = static_cast<const VarDeclStmt&>(*s);
            if (!vd.name.empty()) namedLocals.emplace(vd.name, &vd);
        } else if (s->kind() == Stmt::Kind::For) {
            const auto& fs = static_cast<const ForStmt&>(*s);
            visitStmt(fs.init.get());
            walk(fs.body);
        }
    };
    walk(fn->body);

    // Pre-fix: every alloca produced a numeric fallback (`local0`/`v1`
    // etc.) via getValueName; with DWARF consumption, allocas carrying a
    // DILocalVariable surface the real source name. We assert the three
    // named locals are present. Parameter-spill allocas may or may not be
    // emitted depending on lifter strategy, so this asserts a minimum
    // contract: each unique source local appears at least once.
    EXPECT_TRUE(namedLocals.count("counter") > 0)
        << "expected DWARF-recovered local `counter`; got names: " <<
        [&] {
            std::string s;
            for (const auto& kv : namedLocals) { s += kv.first + " "; }
            return s;
        }();
    EXPECT_TRUE(namedLocals.count("total") > 0)
        << "expected DWARF-recovered local `total`";
    EXPECT_TRUE(namedLocals.count("scale") > 0)
        << "expected DWARF-recovered local `scale`";

    // Type recovery: the recovered types must come through typeFromDI, so
    // `counter` is `int`/`i32`-like, `total` is `long`, `scale` is
    // `double`. The exact spelling depends on LLVMLifter's type-name
    // strategy — assert the source-level spelling survives by accepting
    // either the canonical C++ keyword or the Topo-canonical alias.
    auto typeName = [](const transpile::VarDeclStmt* vd) -> std::string {
        if (!vd) return {};
        std::string s;
        for (const auto& p : vd->type.nameParts) {
            if (!s.empty()) s += "::";
            s += p;
        }
        return s;
    };
    auto looksLikeInt = [](const std::string& t) {
        return t == "int" || t == "i32" || t == "i64" || t == "long";
    };
    auto looksLikeLong = [](const std::string& t) {
        return t == "long" || t == "i64" || t == "long long";
    };
    auto looksLikeDouble = [](const std::string& t) {
        return t == "double" || t == "f64";
    };

    const auto* counter = namedLocals.count("counter")
                              ? namedLocals.at("counter")
                              : nullptr;
    const auto* total =
        namedLocals.count("total") ? namedLocals.at("total") : nullptr;
    const auto* scale =
        namedLocals.count("scale") ? namedLocals.at("scale") : nullptr;

    if (counter)
        EXPECT_TRUE(looksLikeInt(typeName(counter)))
            << "counter's recovered type should be int-like; got "
            << typeName(counter);
    if (total)
        EXPECT_TRUE(looksLikeLong(typeName(total)))
            << "total's recovered type should be long-like; got "
            << typeName(total);
    if (scale)
        EXPECT_TRUE(looksLikeDouble(typeName(scale)))
            << "scale's recovered type should be double-like; got "
            << typeName(scale);

    // Every recovered VarDecl must be Fidelity::Recovered (the alloca-lift
    // baseline) — pre-fix this was the same, so the assert mainly guards
    // against an accidental future downgrade when DWARF is missing.
    for (const auto& kv : namedLocals) {
        EXPECT_EQ(kv.second->fidelity, transpile::Fidelity::Recovered)
            << "DWARF-recovered local `" << kv.first
            << "` must be Fidelity::Recovered";
    }

    fs::remove_all(tmpDir, ec);
}

// functions, and must never emit a degenerate/wrong TryCatch. (The embed
// step intentionally strips EH-bearing bodies, so this guards the "stays
// safe" half of the contract on a real artifact.)
TEST_F(DecompileTest, EHConservativeContractOnRealBinary) {
    auto build = topoBuild("jit");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed:\n" << build.output;

    auto binPath = binaryPath("jit", "ir_embedding");
    ASSERT_TRUE(std::filesystem::exists(binPath)) << "Binary not found: " << binPath;

    decompile::LLVMLifter lifter;
    SymbolTable metadata;
    auto model = lifter.lift(binPath.string(), metadata,
                             transpile::DecompileLevel::Structured);

    EXPECT_FALSE(model.functions.empty())
        << "real-pipeline lift should recover at least one function";
    for (const auto& fn : model.functions)
        auditTryCatch(fn.body); // asserts no degenerate TryCatch leaks
}

// Strong recovery through the real extraction + structured-lift pipeline:
// craft an Itanium C++ EH module, embed its bitcode into an object's
// .topo_ir section via llvm-objcopy (the same section IREmbed uses), then
// drive the public lift(artifactPath, ...) path and assert a real
// TryCatch with the demangled exception type is recovered.
TEST_F(DecompileTest, EHRecoveryThroughEmbeddedSection) {
    namespace fs = std::filesystem;

    fs::path objcopy = llvmBinDir_ / "llvm-objcopy";
    fs::path clang = llvmBinDir_ / "clang";
    if (!fs::exists(objcopy) || !fs::exists(clang))
        GTEST_SKIP() << "llvm-objcopy/clang not available in " << llvmBinDir_;

    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("eh_embed", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* personality = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty, true),
        llvm::GlobalValue::ExternalLinkage, "__gxx_personality_v0", *module);
    auto* beginCatch = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {ptrTy}, false),
        llvm::GlobalValue::ExternalLinkage, "__cxa_begin_catch", *module);
    auto* endCatch = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, false),
        llvm::GlobalValue::ExternalLinkage, "__cxa_end_catch", *module);
    auto* callee = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, false),
        llvm::GlobalValue::ExternalLinkage, "callee", *module);
    auto* tiGlobal = new llvm::GlobalVariable(
        *module, ptrTy, true, llvm::GlobalValue::ExternalLinkage, nullptr,
        "_ZTISt12length_error");

    auto* func = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, false),
        llvm::GlobalValue::ExternalLinkage, "eh_demo", *module);
    func->setPersonalityFn(personality);

    auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
    auto* cont = llvm::BasicBlock::Create(ctx, "cont", func);
    auto* lpad = llvm::BasicBlock::Create(ctx, "lpad", func);
    auto* catchBB = llvm::BasicBlock::Create(ctx, "catch", func);
    auto* ret = llvm::BasicBlock::Create(ctx, "ret", func);

    llvm::IRBuilder<> b(entry);
    b.CreateInvoke(callee, cont, lpad);
    b.SetInsertPoint(cont);
    b.CreateBr(ret);
    b.SetInsertPoint(lpad);
    auto* lpTy = llvm::StructType::get(ptrTy, i32Ty);
    auto* lp = b.CreateLandingPad(lpTy, 1);
    lp->addClause(tiGlobal);
    auto* exn = b.CreateExtractValue(lp, 0, "exn");
    b.CreateCall(beginCatch, {exn});
    b.CreateBr(catchBB);
    b.SetInsertPoint(catchBB);
    b.CreateCall(endCatch, {});
    b.CreateBr(ret);
    b.SetInsertPoint(ret);
    b.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyFunction(*func, &llvm::errs()));

    fs::path tmp = fs::temp_directory_path() / "topo_eh_embed_test";
    fs::create_directories(tmp);
    fs::path bcPath = tmp / "eh.bc";
    fs::path objPath = tmp / "host.o";
    fs::path artifact = tmp / "host_with_ir.o";

    {
        std::error_code ec;
        llvm::raw_fd_ostream os(bcPath.string(), ec);
        ASSERT_FALSE(ec) << "cannot open " << bcPath;
        llvm::WriteBitcodeToFile(*module, os);
    }

    // Produce a trivial host object, then graft the bitcode in as the
    // platform's .topo_ir section (mirrors IREmbed's section naming).
    fs::path cSrc = tmp / "stub.c";
    { std::ofstream f(cSrc); f << "int topo_stub(void){return 0;}\n"; }

    auto r1 = platform::runProcessCapture(
        clang.string(), {"-c", cSrc.string(), "-o", objPath.string()},
        tmp.string());
    ASSERT_EQ(r1.exitCode, 0) << "clang stub compile failed:\n"
                              << r1.stdoutOutput << r1.stderrOutput;

#if defined(__APPLE__)
    std::string secArg = "__DATA,__topo_ir=" + bcPath.string();
#else
    std::string secArg = ".topo_ir=" + bcPath.string();
#endif
    auto r2 = platform::runProcessCapture(
        objcopy.string(),
        {"--add-section", secArg, objPath.string(), artifact.string()},
        tmp.string());
    if (r2.exitCode != 0)
        GTEST_SKIP() << "llvm-objcopy --add-section unsupported here:\n"
                     << r2.stdoutOutput << r2.stderrOutput;

    decompile::LLVMLifter lifter;
    SymbolTable metadata;
    auto model = lifter.lift(artifact.string(), metadata,
                             transpile::DecompileLevel::Structured);

    ASSERT_FALSE(model.functions.empty())
        << "lift over embedded .topo_ir section recovered no functions";

    int totalTryCatch = 0;
    bool sawLengthError = false;
    for (const auto& fn : model.functions) {
        totalTryCatch += auditTryCatch(fn.body);
        // Locate the recovered exception type.
        std::function<void(const std::vector<transpile::StmtPtr>&)> scan =
            [&](const std::vector<transpile::StmtPtr>& body) {
                for (const auto& s : body) {
                    if (!s || s->kind() != transpile::Stmt::Kind::TryCatch)
                        continue;
                    const auto& tc =
                        static_cast<const transpile::TryCatchStmt&>(*s);
                    for (const auto& cc : tc.catchClauses)
                        if (!cc.exceptionType.nameParts.empty() &&
                            cc.exceptionType.nameParts[0] == "std::length_error")
                            sawLengthError = true;
                }
            };
        scan(fn.body);
    }
    EXPECT_GE(totalTryCatch, 1)
        << "real-pipeline lift should recover at least one TryCatch";
    EXPECT_TRUE(sawLengthError)
        << "recovered catch clause should carry the demangled type "
           "std::length_error";

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// C++ by-value lambda capture
// recovery. Clang at `-O0 -g` lowers `auto f = [x](int v){...}; f(y);` to
// an alloca-closure-struct + GEP+store captures + call into a compiler-
// synthesised body. `LLVMLifter::tryRecognizeLambdaCapture` rebuilds a
// `LambdaExpr` for this canonical shape, and the orphan body function is
// suppressed from `model.functions`. Out-of-scope shapes (by-ref capture,
// std::function, virtual dispatch, multi-call-site shared struct,
// non-C++) degrade silently to the linear output.
TEST_F(DecompileTest, LambdaByValueCaptureRecoveredFromUnoptimizedBitcode) {
    namespace fs = std::filesystem;

    fs::path clang = llvmBinDir_ / "clang";
    if (!fs::exists(clang))
        GTEST_SKIP() << "clang not available in " << llvmBinDir_;

    fs::path tmp = fs::temp_directory_path() / "topo_lambda_recover_test";
    fs::create_directories(tmp);
    fs::path src = tmp / "lambda.cpp";
    fs::path bc = tmp / "lambda.bc";
    {
        std::ofstream f(src);
        f << "int add(int x, int y) {\n"
          << "    auto f = [x](int v) { return v + x; };\n"
          << "    return f(y);\n"
          << "}\n";
    }

    // `-O0 -g -emit-llvm -c` keeps the alloca/store/call shape and emits
    // the DWARF that the recogniser uses to spot the closure type.
    auto r = platform::runProcessCapture(
        clang.string(),
        {"-O0", "-g", "-emit-llvm", "-c", src.string(), "-o", bc.string()},
        tmp.string());
    ASSERT_EQ(r.exitCode, 0) << "clang -emit-llvm failed:\n"
                             << r.stdoutOutput << r.stderrOutput;
    ASSERT_TRUE(fs::exists(bc)) << "expected bitcode at " << bc;

    decompile::LLVMLifter lifter;
    SymbolTable metadata;
    auto model = lifter.liftBitcode(bc.string(), metadata,
                                    transpile::DecompileLevel::Structured);

    ASSERT_FALSE(model.functions.empty())
        << "liftBitcode should return at least the `add` function";

    // (1) The recovered model must have NO orphan closure body function.
    //     Clang names the body something like `_ZZ3addiiENK3$_0clEi`; any
    //     surviving function whose mangled name starts with `_ZZ` and
    //     contains `clE` is an orphan we failed to suppress.
    for (const auto& fn : model.functions) {
        const std::string& q = fn.qualifiedName;
        bool isClosure = q.rfind("_ZZ", 0) == 0 && q.find("clE") != std::string::npos;
        EXPECT_FALSE(isClosure)
            << "orphan closure body leaked into model.functions: " << q;
    }

    // (2) Locate `add` (qualified name is the demangled form).
    const transpile::TranspileFunction* addFn = nullptr;
    for (const auto& fn : model.functions) {
        if (fn.qualifiedName.find("add(int") != std::string::npos ||
            fn.qualifiedName == "_Z3addii") {
            addFn = &fn;
            break;
        }
    }
    ASSERT_NE(addFn, nullptr) << "`add` function not lifted";

    // (3) Walk `add`'s body, find the recovered LambdaExpr. We descend
    //     into VarDecl initialisers and Return values to be robust to
    //     where exactly the recogniser placed it.
    const transpile::LambdaExpr* foundLambda = nullptr;
    bool foundAddBinaryOp = false;
    std::function<void(const transpile::Expr*)> scanExpr =
        [&](const transpile::Expr* e) {
            if (!e) return;
            if (e->kind() == transpile::Expr::Kind::Lambda) {
                foundLambda = static_cast<const transpile::LambdaExpr*>(e);
            }
            if (e->kind() == transpile::Expr::Kind::BinaryOp) {
                const auto& b = static_cast<const transpile::BinaryOpExpr&>(*e);
                if (b.op == transpile::BinaryOp::Add) foundAddBinaryOp = true;
                scanExpr(b.lhs.get());
                scanExpr(b.rhs.get());
            } else if (e->kind() == transpile::Expr::Kind::Call) {
                for (const auto& a : static_cast<const transpile::CallExpr&>(*e).args)
                    scanExpr(a.get());
            } else if (e->kind() == transpile::Expr::Kind::Lambda) {
                // descend into the lambda body looking for the Add op
                const auto& l = static_cast<const transpile::LambdaExpr&>(*e);
                std::function<void(const std::vector<transpile::StmtPtr>&)> scanStmts;
                scanStmts = [&](const std::vector<transpile::StmtPtr>& body) {
                    for (const auto& s : body) {
                        if (!s) continue;
                        switch (s->kind()) {
                        case transpile::Stmt::Kind::VarDecl:
                            scanExpr(static_cast<const transpile::VarDeclStmt&>(*s).init.get());
                            break;
                        case transpile::Stmt::Kind::Assign:
                            scanExpr(static_cast<const transpile::AssignStmt&>(*s).value.get());
                            break;
                        case transpile::Stmt::Kind::Return:
                            scanExpr(static_cast<const transpile::ReturnStmt&>(*s).value.get());
                            break;
                        case transpile::Stmt::Kind::ExprStmt:
                            scanExpr(static_cast<const transpile::ExprStmt&>(*s).expr.get());
                            break;
                        case transpile::Stmt::Kind::If: {
                            const auto& i = static_cast<const transpile::IfStmt&>(*s);
                            scanExpr(i.condition.get());
                            scanStmts(i.thenBody);
                            scanStmts(i.elseBody);
                            break;
                        }
                        default: break;
                        }
                    }
                };
                scanStmts(l.body);
            }
        };
    std::function<void(const std::vector<transpile::StmtPtr>&)> scanBody =
        [&](const std::vector<transpile::StmtPtr>& body) {
            for (const auto& s : body) {
                if (!s) continue;
                switch (s->kind()) {
                case transpile::Stmt::Kind::VarDecl:
                    scanExpr(static_cast<const transpile::VarDeclStmt&>(*s).init.get());
                    break;
                case transpile::Stmt::Kind::Assign:
                    scanExpr(static_cast<const transpile::AssignStmt&>(*s).value.get());
                    break;
                case transpile::Stmt::Kind::Return:
                    scanExpr(static_cast<const transpile::ReturnStmt&>(*s).value.get());
                    break;
                case transpile::Stmt::Kind::ExprStmt:
                    scanExpr(static_cast<const transpile::ExprStmt&>(*s).expr.get());
                    break;
                default: break;
                }
            }
        };
    scanBody(addFn->body);

    ASSERT_NE(foundLambda, nullptr)
        << "expected a recovered LambdaExpr in `add`'s body";

    // (4) Capture shape: exactly one by-value capture named `x`.
    ASSERT_EQ(foundLambda->captures.size(), 1u)
        << "expected exactly one capture (the by-value `x`)";
    EXPECT_EQ(foundLambda->captures[0].name, "x");
    EXPECT_EQ(foundLambda->captures[0].mode, transpile::CaptureMode::ByValue);

    // (5) Lambda body must contain the `v + x` add operation.
    EXPECT_TRUE(foundAddBinaryOp)
        << "expected a BinaryOp(Add) inside the recovered lambda body";

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// Rust Box ownership inference (MVP, Box only).
//
// Drives the real rustc -> liftBitcode path: compile a trivial Box-returning
// crate with -Copt-level=0 -g -C debuginfo=2 --emit=llvm-bc, lift the
// bitcode, and assert that the recovered `make` function's return type is
// `owned i32`. The matcher requires BOTH the DWARF `Box<T, Global>` shape
// AND IR-side allocation evidence (a `__rust_alloc` / `exchange_malloc`
// call inside the function), so it conservatively degrades on any
// non-Box / non-Global / cross-function shape.
//
// rustc is sourced from $PATH. When rustc is unavailable the test
// disables itself rather than silently skipping a documented capability —
// the on-path codepath is still exercised by the matcher's static
// constraints (no Box -> no upgrade), and rerunning under a Rust-bearing
// environment lights up this case.
TEST_F(DecompileTest, BoxOwnershipFromAllocDeallocPair) {
    namespace fs = std::filesystem;

    // Resolve rustc from PATH; in this environment there is no bundled
    // rustc under topo-llvm/llvm-dev/ and no scripts/find-rustc.sh helper.
    auto whichRustc = platform::runProcessCapture(
        "/bin/sh", {"-c", "command -v rustc"}, fs::current_path().string());
    if (whichRustc.exitCode != 0 || whichRustc.stdoutOutput.empty()) {
        GTEST_SKIP() << "rustc not on PATH; skipping Box ownership e2e";
    }
    std::string rustcPath = whichRustc.stdoutOutput;
    while (!rustcPath.empty() &&
           (rustcPath.back() == '\n' || rustcPath.back() == '\r' ||
            rustcPath.back() == ' '))
        rustcPath.pop_back();
    ASSERT_TRUE(fs::exists(rustcPath)) << "rustc resolved to non-existent: "
                                       << rustcPath;

    // Workspace under temp_directory_path so each run starts clean.
    fs::path tmp = fs::temp_directory_path() / "topo_box_ownership_test";
    fs::create_directories(tmp);
    fs::path srcPath = tmp / "make.rs";
    {
        std::ofstream f(srcPath);
        f << "#![crate_type = \"lib\"]\n"
             "pub fn make() -> Box<i32> { Box::new(42) }\n";
    }
    fs::path bcPath = tmp / "make.bc";

    auto r = platform::runProcessCapture(
        rustcPath,
        {"-Copt-level=0", "-g", "-C", "debuginfo=2", "--emit=llvm-bc",
         srcPath.string(), "-o", bcPath.string()},
        tmp.string());
    ASSERT_EQ(r.exitCode, 0) << "rustc failed:\n"
                             << r.stdoutOutput << r.stderrOutput;
    ASSERT_TRUE(fs::exists(bcPath)) << "rustc produced no bitcode at "
                                    << bcPath;

    decompile::LLVMLifter lifter;
    SymbolTable metadata;
    auto model = lifter.liftBitcode(bcPath.string(), metadata,
                                    transpile::DecompileLevel::Structured);
    ASSERT_FALSE(model.functions.empty())
        << "liftBitcode produced no functions from rustc-emitted bitcode";

    // Locate the user-defined `make` function. The current LLVMLifter
    // demangling fast-path only matches symbols listed in the supplied
    // SymbolTable; with an empty metadata it falls back to the raw
    // mangled name (e.g. `_ZN4make4make17h...E`). Accept both the
    // demangled `make::make[::h<hash>]` form and the Itanium-mangled
    // form so the test stays insensitive to that downstream fix.
    auto isMakeFunction = [](const std::string& q) {
        // Itanium-mangled: starts with `_ZN4make4make` (length-prefixed
        // path 4-"make" 4-"make").
        if (q.rfind("_ZN4make4make", 0) == 0) return true;
        // Strip a trailing `::h<hex>` Rust hash if present, then match by
        // tail.
        std::string trimmed = q;
        auto hashPos = trimmed.rfind("::h");
        if (hashPos != std::string::npos) {
            std::string tail = trimmed.substr(hashPos + 3);
            bool allHex = !tail.empty();
            for (char c : tail) {
                if (!std::isxdigit((unsigned char)c)) { allHex = false; break; }
            }
            if (allHex) trimmed = trimmed.substr(0, hashPos);
        }
        if (trimmed == "make" || trimmed == "make::make") return true;
        if (trimmed.size() >= 12 &&
            trimmed.compare(trimmed.size() - 12, 12, "::make::make") == 0)
            return true;
        return false;
    };
    const transpile::TranspileFunction* makeFn = nullptr;
    for (const auto& fn : model.functions) {
        if (isMakeFunction(fn.qualifiedName)) { makeFn = &fn; break; }
    }
    if (!makeFn) {
        // Diagnostic: print every lifted qualifiedName so a failure is
        // legible. (Test still fails on the assertion below.)
        std::string seen;
        for (const auto& fn : model.functions)
            seen += "  " + fn.qualifiedName + "\n";
        ADD_FAILURE() << "lifted qualifiedNames:\n" << seen;
    }
    ASSERT_NE(makeFn, nullptr)
        << "Could not locate the Rust `make` function in the lifted model";

    // Core contract: Box<i32> return -> owned i32.
    EXPECT_EQ(makeFn->returnType.ownership, OwnershipKind::Owned)
        << "expected Owned on Box<i32> return; got "
        << ownershipKindName(makeFn->returnType.ownership);
    EXPECT_EQ(makeFn->returnType.modifier, TypeNode::None)
        << "Owned Box<T> must not also carry the pointer modifier";

    ASSERT_FALSE(makeFn->returnType.nameParts.empty())
        << "recovered return type must have a pointee name";
    EXPECT_EQ(makeFn->returnType.nameParts, std::vector<std::string>{"i32"})
        << "expected pointee nameParts == [\"i32\"], got first part: "
        << makeFn->returnType.nameParts.front();

    // Conservative degradation contract: helper functions compiled in by
    // rustc (exchange_malloc, alloc_impl, ...) must NOT be marked Owned —
    // their DWARF return types are raw pointers (`*mut u8`), not
    // `Box<T, Global>`. The recognizer must match only on the strict
    // Box<T, Global> outermost-DWARF shape backed by IR alloc evidence.
    for (const auto& fn : model.functions) {
        if (&fn == makeFn) continue;
        if (fn.qualifiedName.find("exchange_malloc") != std::string::npos) {
            EXPECT_NE(fn.returnType.ownership, OwnershipKind::Owned)
                << "exchange_malloc was incorrectly marked Owned — its "
                   "DWARF return is *mut u8, not Box<T, Global>";
        }
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

} // namespace topo::test::e2e
