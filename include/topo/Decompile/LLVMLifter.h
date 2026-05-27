#ifndef TOPO_DECOMPILE_LLVMLIFTER_H
#define TOPO_DECOMPILE_LLVMLIFTER_H

#include "topo/Transpile/BackendLifter.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Analysis/PostDominators.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace topo::decompile {

class LLVMLifter : public transpile::BackendLifter {
public:
    transpile::TranspileModule lift(const std::string& artifactPath,
                                    const SymbolTable& metadata,
                                    transpile::DecompileLevel level) override;

    // Lift a standalone LLVM bitcode (.bc) or textual IR (.ll) file. Distinct
    // from lift(), which extracts pre-codegen IR from a linked binary's
    // `.topo_ir` section after the O2 PassPipeline has run. This entry takes
    // raw IR directly, so when the .bc was produced by `clang -g -O0 -emit-llvm`
    // (or `rustc -g --emit=llvm-bc -Copt-level=0`) the DWARF metadata still
    // hangs off live alloca / struct types and the DWARF recovery infra
    // (struct field names, local variable types, closure capture pattern,
    // Box<T> ownership) can be exercised end-to-end — the path that is
    // GATE-blocked for embedded IR (the post-O2 mem2reg + named-struct
    // elimination strips the consumable structure from the embedded snapshot).
    transpile::TranspileModule liftBitcode(const std::string& bitcodePath,
                                           const SymbolTable& metadata,
                                           transpile::DecompileLevel level);

    // Per-function lifting (public for unit testing)
    transpile::TranspileFunction liftFunctionDirect(const llvm::Function& func,
                                                    const std::string& qualifiedName,
                                                    const SymbolTable& metadata);

    transpile::TranspileFunction liftFunctionStructured(llvm::Function& func,
                                                        const std::string& qualifiedName,
                                                        const SymbolTable& metadata);

private:
    // Drive lifting once an `llvm::Module` is in hand. Shared by `lift()`
    // (embedded `.topo_ir` section path) and `liftBitcode()` (raw `.bc`).
    transpile::TranspileModule liftModule(llvm::Module& module,
                                          const SymbolTable& metadata,
                                          transpile::DecompileLevel level);

    // Extract embedded pre-codegen IR from binary (.topo_ir section)
    std::unique_ptr<llvm::Module> extractIR(const std::string& artifactPath, llvm::LLVMContext& context);


    // Convert an LLVM instruction to a statement
    transpile::StmtPtr liftInstruction(const llvm::Instruction& inst);

    // Convert an LLVM value to an expression
    transpile::ExprPtr liftValue(const llvm::Value& val);

    // Map LLVM type to TypeNode
    TypeNode liftType(const llvm::Type* type);

    // --- DWARF debug-info recovery ---
    // Build module-level DWARF maps once per lift(). Conservative: when the
    // embedded IR carries no debug info these stay empty and every consumer
    // degrades to the pre-existing numeric / liftType behavior.
    void buildDebugInfoMaps(llvm::Module& module);

    // Recover ordered member names for a struct/class, keyed by the LLVM
    // identified-struct name (prefixes stripped). Empty when no DICompositeType
    // matched. Member i absent -> caller falls back to "field{i}".
    const std::vector<std::string>* debugMemberNames(llvm::StringRef structName) const;

    // Recover a TypeNode from a DWARF DIType (strips typedef/const, follows
    // pointer/reference). Returns nullopt when the DIType is null/unhandled so
    // the caller keeps its liftType result.
    std::optional<TypeNode> typeFromDI(const llvm::DIType* diTy) const;

    // DILocalVariable attached (via dbg.declare / dbg_value) to an alloca.
    const llvm::DILocalVariable* debugLocalForAlloca(const llvm::AllocaInst* a) const;

    // --- Rust Box ownership recovery ----------------------------------------
    //
    // If `diTy` is a `DW_TAG_pointer_type` whose name describes a Rust
    // `alloc::boxed::Box<T, ...>` AND `func` provides corroborating IR
    // evidence (a direct/inlined `__rust_alloc` site, or — for the Box::new
    // lowering — a call to `alloc::alloc::exchange_malloc`, the well-known
    // single intermediate that wraps `__rust_alloc`), recover the inner T
    // via `typeFromDI()` and return a TypeNode with `ownership = Owned`
    // and `modifier = None`.
    //
    // Conservative by construction — returns `std::nullopt` for any shape
    // that cannot be confidently classified as Box<T> with a recovered
    // pointee:
    //   * non-`Box<...>` smart-pointer wrappers (`Arc`, `Rc`, `Weak`, etc.);
    //   * `Box<Box<...>>` (nested Box — keep degraded);
    //   * custom-allocator Box (we only accept `alloc::alloc::Global`);
    //   * pointee cannot be recovered from DWARF;
    //   * no IR allocation evidence in `func` (would force cross-function
    //     inference, which is explicitly out of scope).
    //
    // `func` may be null when no enclosing function is known (e.g. type
    // lifting outside a function context); in that case the IR-pairing
    // check cannot succeed and the helper degrades to `std::nullopt`.
    std::optional<TypeNode> recoverBoxOwnership(const llvm::DIType* diTy,
                                                const llvm::Function* func) const;

    // True iff `func`'s body contains an instruction that pairs the Box<T>
    // shape: a direct/inlined call to `__rust_alloc` (whose result escapes
    // via return or is freed by a sibling `__rust_dealloc`), OR a call to
    // the documented Box::new lowering intermediate `exchange_malloc`.
    // Used as the IR-side gate for `recoverBoxOwnership` so DWARF alone is
    // never sufficient to claim Owned.
    static bool functionHasBoxAllocEvidence(const llvm::Function& func);

    // structName (prefix-stripped) -> ordered member names
    std::unordered_map<std::string, std::vector<std::string>> diStructMembers_;
    // alloca -> DILocalVariable
    std::unordered_map<const llvm::AllocaInst*, const llvm::DILocalVariable*> diLocals_;
    bool hasDebugInfo_ = false;

    // Get a readable name for an LLVM value, generating a temp name if needed
    std::string getValueName(const llvm::Value& val);

    // Map LLVM binary opcode to TranspileModel BinaryOp
    static transpile::BinaryOp mapBinaryOp(unsigned opcode);

    // Map LLVM ICmp predicate to TranspileModel BinaryOp
    static transpile::BinaryOp mapICmpPredicate(llvm::CmpInst::Predicate pred);

    // Map LLVM FCmp predicate to TranspileModel BinaryOp
    static transpile::BinaryOp mapFCmpPredicate(llvm::CmpInst::Predicate pred);

    // Lift a region of basic blocks into statements with CFG reconstruction
    void liftBlockRegion(llvm::BasicBlock* entry,
                         llvm::BasicBlock* exit,
                         llvm::LoopInfo& LI,
                         llvm::PostDominatorTree& PDT,
                         std::vector<transpile::StmtPtr>& stmts,
                         std::unordered_map<llvm::BasicBlock*, bool>& visited);

    // Detect if a loop is a simple counted loop (induction var + bound)
    bool detectCountedLoop(llvm::Loop* loop,
                           transpile::StmtPtr& initStmt,
                           transpile::ExprPtr& condition,
                           transpile::ExprPtr& increment);

    // --- Itanium C++ exception-handling recovery ---------------------------
    //
    // Recognize the `invoke` / `landingpad` / `resume` shape produced by
    // clang for the Itanium C++ ABI (`__gxx_personality_v0`) and rebuild a
    // TranspileModel TryCatchStmt. Conservative by construction: any shape
    // that does not cleanly map (SEH/funclet EH, non-gxx personality,
    // nested/overlapping regions we cannot delimit) is left to the normal
    // per-instruction / structured path — never a wrong or empty TryCatch.

    // True iff the function uses the Itanium C++ personality and so its
    // invoke/landingpad shape is a candidate for EH recovery.
    static bool usesItaniumCxxEH(const llvm::Function& func);

    // If `entry` begins an Itanium C++ try region (i.e. it transitively
    // reaches an InvokeInst before leaving the region), reconstruct a
    // TryCatchStmt into `stmts` and return true. The protected region runs
    // from `entry` up to (but excluding) the post-dominator that the
    // invoke's normal edge converges with the landingpad cleanup-resume.
    // Returns false (touching nothing) when the shape is not a clean,
    // single, confidently-delimited C++ EH region.
    bool tryLiftEHRegion(llvm::BasicBlock* entry,
                         llvm::BasicBlock* exit,
                         llvm::LoopInfo& LI,
                         llvm::PostDominatorTree& PDT,
                         std::vector<transpile::StmtPtr>& stmts,
                         std::unordered_map<llvm::BasicBlock*, bool>& visited);

    // Resolve a `landingpad`'s clauses into CatchClause list + a flag for
    // whether a cleanup (finally) path is present. Returns false if the
    // landingpad shape is not understood (caller must then bail out).
    bool liftLandingPad(llvm::LandingPadInst* lp,
                        llvm::BasicBlock* lpadBB,
                        llvm::BasicBlock* regionExit,
                        llvm::LoopInfo& LI,
                        llvm::PostDominatorTree& PDT,
                        std::vector<transpile::CatchClause>& catches,
                        std::vector<transpile::StmtPtr>& finallyBody,
                        std::unordered_map<llvm::BasicBlock*, bool>& visited);

    // Demangle an Itanium typeinfo symbol (`_ZTI...`) to a readable type
    // name (e.g. `_ZTISt12length_error` -> `std::length_error`). Returns
    // empty string when it cannot be resolved confidently.
    static std::string typeInfoToReadableName(const llvm::Value* tiGlobal);

    // Temporary variable counter (reset per function)
    unsigned tmpCounter_ = 0;

    // Value -> name mapping for the current function
    std::unordered_map<const llvm::Value*, std::string> nameMap_;

    // EH-region entry blocks whose TryCatch is already being built — guards
    // the pre-pass against re-triggering when it recurses into its own
    // protected region (which starts at the same entry block).
    std::unordered_set<const llvm::BasicBlock*> ehRegionEntries_;

    // --- C++ closure recovery (debug-IR MVP) ---
    //
    // Clang at `-O0 -g` lowers `auto f = [x](int v){ return v+x; }; f(y);`
    // into: alloca a `%class.anon` closure struct, GEP+store each captured
    // value into that struct, then call the closure body function with
    // `&struct` as the first argument. The mem2reg pass erases this shape,
    // so recovery is meaningful only on unoptimised, debug-bearing IR
    // (where the dedicated `liftBitcode` entry point hands the module in
    // directly). When the pattern matches we rebuild a `LambdaExpr` and
    // mark the body function for suppression so the lifted module does not
    // also emit an orphan `operator()` callable. Every shape we cannot
    // confidently recognise (by-ref capture, multi-callsite shared struct,
    // virtual dispatch, std::function, non-C++) degrades silently to the
    // pre-existing linear output — we never produce a wrong `LambdaExpr`.

    // Try to recognise the C++ by-value lambda-call shape at `call`. On
    // success returns a statement that wraps the `LambdaExpr`-typed call
    // expression (an `ExprStmt` for void returns, a `VarDeclStmt` for
    // non-void) and records the closure body in `suppressFromModule_`.
    // On any non-match returns nullptr; the caller then falls through to
    // the normal `CallInst` handling untouched.
    transpile::StmtPtr tryRecognizeLambdaCapture(const llvm::CallInst& call);

    // Functions whose definition should be excluded from `model.functions`
    // because they have been inlined into a recovered `LambdaExpr` body.
    // Cleared at the start of every top-level lift.
    std::unordered_set<const llvm::Function*> suppressFromModule_;
};

} // namespace topo::decompile

#endif // TOPO_DECOMPILE_LLVMLIFTER_H
