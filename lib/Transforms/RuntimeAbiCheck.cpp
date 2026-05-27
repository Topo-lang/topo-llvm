// Shared helper used by the runtime-touching passes — not a pass itself,
// so no @category. The check-feature-taxonomy.sh skip list excludes
// this file from its source-level audit.
#include "topo/Transforms/RuntimeAbiCheck.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <string>

namespace topo {

namespace {

/// Build the diagnostic message the ctor prints to stderr on mismatch.
///
/// Shape mirrors the structured wording the runtime-side reference
/// pattern (`topo_jit_api.cpp:209-215`) implies — name the library,
/// state the observed and expected ABI versions, point at
/// `ABI-COMPAT.md`. The message is materialised as a private constant
/// string in the module and emitted via a single `fputs(msg, stderr)`
/// call so the helper does not depend on `fprintf` variadic codegen.
std::string buildDiagnostic(const std::string& libName, std::uint32_t expectedVersion) {
    std::string msg;
    msg.reserve(128 + libName.size());
    msg += "topo: libtopo-";
    msg += libName;
    msg += " ABI mismatch (expected v";
    msg += std::to_string(expectedVersion);
    msg += "); rebuild caller and runtime from a matching commit. "
           "See topo-llvm/runtime/ABI-COMPAT.md\n";
    return msg;
}

/// Materialise or reuse a private constant null-terminated C string GV
/// keyed by `name`. The pointer-cast return matches the `i8*` shape an
/// `fputs` / external-C function call wants.
llvm::Constant* getOrCreateGlobalCString(llvm::Module& module,
                                         const std::string& name,
                                         const std::string& value) {
    if (auto* existing = module.getNamedGlobal(name)) {
        return llvm::ConstantExpr::getPointerCast(existing,
                                                  llvm::PointerType::getUnqual(module.getContext()));
    }

    auto& ctx = module.getContext();
    auto* strConst = llvm::ConstantDataArray::getString(ctx, value, /*AddNull=*/true);
    auto* gv = new llvm::GlobalVariable(module,
                                        strConst->getType(),
                                        /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        strConst,
                                        name);
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    return llvm::ConstantExpr::getPointerCast(gv,
                                              llvm::PointerType::getUnqual(ctx));
}

} // anonymous namespace

std::string runtimeAbiCheckCtorName(const std::string& libName) {
    return "topo_" + libName + "_abi_check_ctor";
}

void injectAbiCheckCtor(llvm::Module& module,
                        const std::string& libName,
                        const std::string& versionSymbol,
                        std::uint32_t expectedVersion) {
    auto ctorName = runtimeAbiCheckCtorName(libName);

    // Idempotency: if a previous pass on the same module already wired
    // this library's ABI-check ctor, do nothing. This guards the
    // TopoParallelPass + LoopParallelizePass shared `libtopo-parallel`
    // case, and is also cheap insurance against running the same pass
    // twice in a wider pipeline.
    if (module.getNamedValue(ctorName) != nullptr) {
        return;
    }

    auto& ctx = module.getContext();
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);

    // Declare (or reuse) the runtime introspection symbol. Shape is
    // `uint32_t topo_<lib>_version(void)` per `<lib>_rt.h`.
    auto* versionFnTy = llvm::FunctionType::get(i32Ty, {}, /*isVarArg=*/false);
    auto versionCallee = module.getOrInsertFunction(versionSymbol, versionFnTy);

    // Diagnostic shape: `write(STDERR_FILENO, msg, len)` then `abort()`.
    //
    // Why `write` and not `fputs(msg, stderr)`: `stderr` is a macro on
    // many libc implementations (notably Apple libc, where it expands
    // to `__stderrp` — there is no `_stderr` external symbol). The
    // POSIX `write(2)` syscall takes a file descriptor literal, needs
    // no symbol indirection through libc-private FILE*, and is
    // available on every Unix-y platform the runtime targets. On
    // Windows the same shape lowers through `_write` from the CRT.
    // This keeps the helper independent of libc internals and lets
    // the emitted IR link against the same runtime libraries the
    // optimized binary already drags in.
    auto* sizeTy = i64Ty;
    auto* writeFnTy =
        llvm::FunctionType::get(sizeTy, {i32Ty, ptrTy, sizeTy}, /*isVarArg=*/false);
    auto writeCallee = module.getOrInsertFunction("write", writeFnTy);

    // Declare `void abort(void) __attribute__((noreturn))`.
    auto* abortFnTy = llvm::FunctionType::get(voidTy, {}, /*isVarArg=*/false);
    auto abortCallee = module.getOrInsertFunction("abort", abortFnTy);
    if (auto* abortFn = llvm::dyn_cast<llvm::Function>(abortCallee.getCallee())) {
        abortFn->addFnAttr(llvm::Attribute::NoReturn);
        abortFn->addFnAttr(llvm::Attribute::NoUnwind);
    }

    // Synthesize the ctor function. Internal linkage — there is no need
    // for the symbol to be visible outside the module. Naming uses the
    // pinned suffix from `runtimeAbiCheckCtorName` so the idempotency
    // probe above finds it on a second call.
    auto* ctorFnTy = llvm::FunctionType::get(voidTy, {}, /*isVarArg=*/false);
    auto* ctorFn = llvm::Function::Create(ctorFnTy,
                                          llvm::GlobalValue::InternalLinkage,
                                          ctorName,
                                          &module);

    auto* entry = llvm::BasicBlock::Create(ctx, "entry", ctorFn);
    auto* mismatch = llvm::BasicBlock::Create(ctx, "mismatch", ctorFn);
    auto* okBlock = llvm::BasicBlock::Create(ctx, "ok", ctorFn);

    {
        llvm::IRBuilder<> b(entry);
        auto* observed = b.CreateCall(versionCallee, {}, "abi.observed");
        auto* expected = llvm::ConstantInt::get(i32Ty, expectedVersion);
        auto* ne = b.CreateICmpNE(observed, expected, "abi.mismatch");
        b.CreateCondBr(ne, mismatch, okBlock);
    }

    {
        llvm::IRBuilder<> b(mismatch);
        auto diag = buildDiagnostic(libName, expectedVersion);
        auto* msg = getOrCreateGlobalCString(module,
                                             ".str.topo_abi." + libName,
                                             diag);
        // write(STDERR_FILENO=2, msg, strlen(msg))
        auto* fd = llvm::ConstantInt::get(i32Ty, 2);
        auto* len = llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(diag.size()));
        b.CreateCall(writeCallee, {fd, msg, len});
        b.CreateCall(abortCallee, {});
        b.CreateUnreachable();
    }

    {
        llvm::IRBuilder<> b(okBlock);
        b.CreateRetVoid();
    }

    // Register with the standard LLVM global-ctor table. Priority 0
    // matches the JIT-engine reference pattern's intent (run before any
    // user code that might call into the library).
    llvm::appendToGlobalCtors(module, ctorFn, /*Priority=*/0);
}

} // namespace topo
