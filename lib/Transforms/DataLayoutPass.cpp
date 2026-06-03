// @category: ENHANCE
#include "topo/Transforms/DataLayoutPass.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace topo {

namespace {

// ============================================================================
// Step 1: Detection — isFixedSizeArrayOfStruct
// ============================================================================

struct ArrayOfStructInfo {
    llvm::StructType* wrapperType; // %"struct.std::array<Particle, 128>" / topo::array / std::__1::array
    llvm::StructType* elementType; // %struct.Particle
    uint64_t arraySize;            // N
};

/// Check if a StructType is a fixed-size array wrapper of struct elements.
/// Recognizes both `topo::array<T, N>` (the public alias) and the underlying
/// `std::array<T, N>` shapes that the alias resolves to:
///   - "struct.topo::array" / "class.topo::array"  (legacy / explicit topo::)
///   - "struct.std::array"  / "class.std::array"   (libstdc++)
///   - "struct.std::__<N>::array" / "class.std::__<N>::array"
///       (libc++ inline namespace, e.g. std::__1::array)
/// Structurally all three forms are identical: exactly one member that is
/// `[N x ElemType]` (libc++ field name `__elems_`, libstdc++ `_M_elems`,
/// historical topo `data_`). Only the name match is loosened — the
/// "exactly one [N x T] member" shape check is preserved.
std::optional<ArrayOfStructInfo> isFixedSizeArrayOfStruct(llvm::StructType* sty) {
    if (!sty || !sty->hasName()) return std::nullopt;

    auto name = sty->getName();
    auto matchesArrayName = [](llvm::StringRef n) {
        if (n.starts_with("struct.topo::array") || n.starts_with("class.topo::array")) return true;
        if (n.starts_with("struct.std::array") || n.starts_with("class.std::array")) return true;
        // libc++ inline-namespace form: struct.std::__<segment>::array
        for (llvm::StringRef prefix : {"struct.std::__", "class.std::__"}) {
            if (!n.starts_with(prefix)) continue;
            llvm::StringRef rest = n.substr(prefix.size());
            // Skip the inline-namespace segment (e.g. "1", "ndk1", "alpha")
            // up to the next "::"; require it to be followed by "array".
            auto sepPos = rest.find("::");
            if (sepPos == llvm::StringRef::npos) continue;
            llvm::StringRef after = rest.substr(sepPos + 2);
            if (after.starts_with("array")) return true;
        }
        return false;
    };
    if (!matchesArrayName(name)) return std::nullopt;

    // Fixed-size array wrapper has exactly one member: the storage array
    if (sty->getNumElements() != 1) return std::nullopt;

    auto* innerTy = sty->getElementType(0);
    auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(innerTy);
    if (!arrTy) return std::nullopt;

    auto* elemTy = llvm::dyn_cast<llvm::StructType>(arrTy->getElementType());
    if (!elemTy) return std::nullopt;

    return ArrayOfStructInfo{sty, elemTy, arrTy->getNumElements()};
}

// ============================================================================
// MSVC-ABI fallback: recover topo::array<T,N> candidates whose wrapper struct
// was elided from the IR.
//
// Under the MSVC ABI + opaque pointers, clang elides the single-member
// `topo::array<T,N>` wrapper struct entirely: it never enters the module's
// identified-struct-type list, so the primary getIdentifiedStructTypes() scan
// finds nothing (only `%struct.<Elem>` survives). The Itanium ABI keeps
// `%"struct.topo::array..."` as a named type, so there the scan succeeds.
//
// The only ABI-stable trace of the container is the mangled function name —
// both Itanium (`topo::array<Particle, 262144ul>`) and Microsoft
// (`struct topo::array<struct Particle, 262144>`) demangle to a readable
// `topo::array<Elem, N>` signature. This helper recovers (Elem, N) from any
// such parameter and resolves Elem against the element struct that IS present
// in the module, synthesizing a wrapper StructType {[N x Elem]} so the rest of
// the pass (which keys off ArrayOfStructInfo) works unchanged. The element/
// field GEP access pattern the transform matches (pattern (b): `[N x Elem]`
// element GEP + typed field GEP) is identical on both ABIs, so once the
// candidate is recovered the transform fires normally.
//
// Gated on a genuine `topo::array<...>` parameter name, so incidental
// fixed-size arrays of unrelated structs are never picked up.

/// Parse the `<Elem, N>` of a demangled `topo::array<Elem, N>` signature.
/// Returns (element-name, N); the element name may stay namespace-qualified.
std::optional<std::pair<std::string, uint64_t>> parseArraySignature(llvm::StringRef demangled) {
    constexpr llvm::StringRef kTag = "topo::array<";
    size_t pos = demangled.find(kTag);
    if (pos == llvm::StringRef::npos) return std::nullopt;

    // Balanced-angle scan from the opening '<' of the template argument list.
    size_t open = pos + kTag.size() - 1; // index of '<'
    int depth = 0;
    size_t commaPos = llvm::StringRef::npos;
    size_t close = llvm::StringRef::npos;
    for (size_t i = open; i < demangled.size(); ++i) {
        char c = demangled[i];
        if (c == '<') {
            ++depth;
        } else if (c == '>') {
            if (--depth == 0) {
                close = i;
                break;
            }
        } else if (c == ',' && depth == 1 && commaPos == llvm::StringRef::npos) {
            commaPos = i; // top-level comma separating Elem from N
        }
    }
    if (close == llvm::StringRef::npos || commaPos == llvm::StringRef::npos) return std::nullopt;

    llvm::StringRef elemPart = demangled.substr(open + 1, commaPos - (open + 1)).trim();
    llvm::StringRef nPart = demangled.substr(commaPos + 1, close - (commaPos + 1)).trim();

    // Strip a leading elaborated-type keyword the MSVC demangler emits.
    for (llvm::StringRef kw : {"struct ", "class ", "union ", "enum "}) {
        if (elemPart.starts_with(kw)) {
            elemPart = elemPart.substr(kw.size()).trim();
            break;
        }
    }
    if (elemPart.empty()) return std::nullopt;

    // N: leading decimal digits (Itanium appends a `ul`/`l` literal suffix).
    uint64_t N = 0;
    bool any = false;
    for (char c : nPart) {
        if (c < '0' || c > '9') break;
        N = N * 10 + static_cast<uint64_t>(c - '0');
        any = true;
    }
    if (!any || N == 0) return std::nullopt;

    return std::make_pair(elemPart.str(), N);
}

/// Resolve a (possibly namespace-qualified) element type name to a StructType
/// in the module's identified-struct list (e.g. "Particle" -> %struct.Particle,
/// "ns::Foo" -> %"struct.ns::Foo").
llvm::StructType* findElementStructByName(llvm::Module& module, llvm::StringRef elemName) {
    llvm::StringRef simple = elemName;
    if (auto p = elemName.rfind(':'); p != llvm::StringRef::npos) simple = elemName.substr(p + 1);

    for (auto* sty : module.getIdentifiedStructTypes()) {
        if (!sty->hasName()) continue;
        llvm::StringRef bare = sty->getName();
        for (llvm::StringRef pre : {"struct.", "class.", "union."}) {
            if (bare.starts_with(pre)) {
                bare = bare.substr(pre.size());
                break;
            }
        }
        if (bare == elemName || bare == simple) return sty;
        // Namespace-qualified IR name ending in "::<simple>".
        if (bare.ends_with(simple) && bare.size() > simple.size() + 2 &&
            bare.substr(bare.size() - simple.size() - 2, 2) == "::")
            return sty;
    }
    return nullptr;
}

/// Recover elided topo::array<Elem,N> candidates from demangled function names.
/// Each distinct (Elem, N) yields one synthetic-wrapper ArrayOfStructInfo.
std::vector<ArrayOfStructInfo> recoverElidedArrayTypes(llvm::Module& module) {
    std::vector<ArrayOfStructInfo> recovered;
    std::set<std::pair<llvm::StructType*, uint64_t>> seen;
    auto& ctx = module.getContext();

    for (auto& func : module) {
        if (!func.hasName()) continue;
        auto sig = parseArraySignature(llvm::demangle(func.getName().str()));
        if (!sig) continue;
        auto* elemTy = findElementStructByName(module, sig->first);
        if (!elemTy) continue;
        if (!seen.insert({elemTy, sig->second}).second) continue;

        auto* arrTy = llvm::ArrayType::get(elemTy, sig->second);
        auto* wrapper = llvm::StructType::create(ctx, {arrTy}, "struct.topo::array.recovered");
        recovered.push_back(ArrayOfStructInfo{wrapper, elemTy, sig->second});
    }
    return recovered;
}

/// Recursively search for topo::array wrapper type usage from a pointer value.
/// Follows GEPs, loads, and call arguments into callees (1 level deep).
std::optional<ArrayOfStructInfo> findWrapperTypeFromUses(llvm::Value* val, int depth = 0) {
    if (depth > 2) return std::nullopt;

    for (auto* user : val->users()) {
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
            if (auto* srcTy = llvm::dyn_cast<llvm::StructType>(gep->getSourceElementType())) {
                if (auto info = isFixedSizeArrayOfStruct(srcTy)) return info;
            }
        } else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user)) {
            if (auto* srcTy = llvm::dyn_cast<llvm::StructType>(load->getType())) {
                if (auto info = isFixedSizeArrayOfStruct(srcTy)) return info;
            }
        } else if (auto* cb = llvm::dyn_cast<llvm::CallBase>(user)) {
            // Follow into callee's corresponding parameter
            auto* callee = cb->getCalledFunction();
            if (!callee || callee->isDeclaration()) continue;
            for (unsigned i = 0; i < cb->arg_size(); ++i) {
                if (cb->getArgOperand(i) == val && i < callee->arg_size()) {
                    auto result = findWrapperTypeFromUses(callee->getArg(i), depth + 1);
                    if (result) return result;
                }
            }
        }
    }
    return std::nullopt;
}

/// Check if a function parameter type is (pointer to) topo::array<Struct, N>.
/// Returns the ArrayOfStructInfo if found.
std::optional<ArrayOfStructInfo> getArrayParamInfo(llvm::Argument* arg) {
    auto* ty = arg->getType();

    // Parameters are typically passed by pointer (reference in C++)
    if (ty->isPointerTy()) return findWrapperTypeFromUses(arg);

    // Direct struct value (unlikely for arrays, but handle it)
    if (auto* sty = llvm::dyn_cast<llvm::StructType>(ty)) return isFixedSizeArrayOfStruct(sty);

    return std::nullopt;
}

// ============================================================================
// Step 2: Field Resolution — resolveStructFields
// ============================================================================

/// Map an IR StructType name to a ClassSymbol in the SymbolTable.
/// Demangling heuristic: strip "struct." prefix, try exact match,
/// then namespace-suffix match.
const ClassSymbol* resolveStructFields(llvm::StructType* elemSty, const SymbolTable& symbols) {
    if (!elemSty || !elemSty->hasName()) return nullptr;

    // Strip "struct." or "class." prefix from IR name
    auto irName = elemSty->getName().str();
    std::string qualifiedName;

    if (irName.substr(0, 7) == "struct.")
        qualifiedName = irName.substr(7);
    else if (irName.substr(0, 6) == "class.")
        qualifiedName = irName.substr(6);
    else
        qualifiedName = irName;

    // Try exact match
    if (auto* cs = symbols.findClassSymbol(qualifiedName)) return cs;

    // Try namespace-suffix match: iterate all class symbols and check if
    // the qualified name ends with our name
    for (const auto& [name, cs] : symbols.classSymbols()) {
        if (name.size() >= qualifiedName.size()) {
            auto pos = name.rfind(qualifiedName);
            if (pos != std::string::npos && pos + qualifiedName.size() == name.size()) {
                // Verify it's a proper namespace boundary
                if (pos == 0 || name[pos - 1] == ':') return &cs;
            }
        }
        // Also try: class symbol name ends with the simple struct name
        auto dotPos = qualifiedName.rfind("::");
        if (dotPos != std::string::npos) {
            auto simpleName = qualifiedName.substr(dotPos + 2);
            if (cs.simpleName == simpleName) return &cs;
        }
    }

    return nullptr;
}

// ============================================================================
// Step 3: Access Analysis — collectArrayFieldReads
// ============================================================================

/// Walk GEP chains from a pointer to a topo::array wrapper and collect
/// which struct field indices are accessed.
///
/// The 3-level GEP access pattern:
///   wrapper (struct.topo::array) → data_ member [0] → element [i] → field [f]
///
/// Handles both collapsed multi-index GEPs and chained single-index GEPs.
void collectArrayFieldReads(llvm::Value* ptr,
                            llvm::StructType* wrapperSty,
                            llvm::StructType* elemSty,
                            std::set<unsigned>& liveFields,
                            std::unordered_set<llvm::Value*>& visited) {
    if (!visited.insert(ptr).second) return;

    unsigned numFields = elemSty->getNumElements();
    auto markAllLive = [&]() {
        for (unsigned i = 0; i < numFields; ++i)
            liveFields.insert(i);
    };

    for (auto* user : ptr->users()) {
        if (liveFields.size() == numFields) return;

        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
            auto* srcTy = gep->getSourceElementType();

            if (srcTy == wrapperSty) {
                // GEP into wrapper: could be collapsed or first level
                unsigned numIdx = gep->getNumIndices();
                if (numIdx >= 4) {
                    // Collapsed GEP: wrapper[0].data_[0][i][f]
                    // Indices: base, 0(data_), i(element), f(field)
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(4))) {
                        unsigned fieldIdx = ci->getZExtValue();
                        if (fieldIdx < numFields)
                            liveFields.insert(fieldIdx);
                        else
                            markAllLive();
                    } else {
                        // Variable field index → conservative
                        markAllLive();
                    }
                } else {
                    // Partial GEP — follow further uses
                    collectArrayFieldReads(gep, wrapperSty, elemSty, liveFields, visited);
                }

            } else if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(srcTy)) {
                // GEP into the [N x Struct] array member
                if (arrTy->getElementType() == elemSty) {
                    unsigned numIdx = gep->getNumIndices();
                    if (numIdx >= 3) {
                        // GEP [N x Struct], ptr, 0, i, f
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(3))) {
                            unsigned fieldIdx = ci->getZExtValue();
                            if (fieldIdx < numFields)
                                liveFields.insert(fieldIdx);
                            else
                                markAllLive();
                        } else {
                            markAllLive();
                        }
                    } else {
                        // GEP into element, follow uses
                        collectArrayFieldReads(gep, wrapperSty, elemSty, liveFields, visited);
                    }
                } else {
                    collectArrayFieldReads(gep, wrapperSty, elemSty, liveFields, visited);
                }

            } else if (srcTy == elemSty) {
                // GEP into a single struct element
                if (gep->getNumIndices() >= 2) {
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2))) {
                        unsigned fieldIdx = ci->getZExtValue();
                        if (fieldIdx < numFields)
                            liveFields.insert(fieldIdx);
                        else
                            markAllLive();
                    } else {
                        markAllLive();
                    }
                } else {
                    collectArrayFieldReads(gep, wrapperSty, elemSty, liveFields, visited);
                }
            } else {
                // Unknown GEP source type — follow uses
                collectArrayFieldReads(gep, wrapperSty, elemSty, liveFields, visited);
            }

        } else if (auto* si = llvm::dyn_cast<llvm::StoreInst>(user)) {
            if (si->getValueOperand() == ptr) {
                // Pointer stored somewhere — follow loads
                llvm::Value* target = si->getPointerOperand();
                for (auto* tUser : target->users()) {
                    if (auto* li = llvm::dyn_cast<llvm::LoadInst>(tUser))
                        collectArrayFieldReads(li, wrapperSty, elemSty, liveFields, visited);
                }
            }
            // ptr as store destination → write, not a field read pattern we track

        } else if (auto* cb = llvm::dyn_cast<llvm::CallBase>(user)) {
            // Pointer passed to another function — recurse
            auto* callee = cb->getCalledFunction();
            if (!callee || callee->isDeclaration()) {
                markAllLive();
                continue;
            }
            for (unsigned i = 0; i < cb->arg_size(); ++i) {
                if (cb->getArgOperand(i) == ptr && i < callee->arg_size())
                    collectArrayFieldReads(callee->getArg(i), wrapperSty, elemSty, liveFields, visited);
            }

        } else if (llvm::isa<llvm::LoadInst>(user)) {
            // Direct load of the whole array pointer — conservative
            markAllLive();

        } else if (llvm::isa<llvm::BitCastInst>(user) || llvm::isa<llvm::AddrSpaceCastInst>(user)) {
            // Cast — follow through
            collectArrayFieldReads(llvm::cast<llvm::Instruction>(user), wrapperSty, elemSty, liveFields, visited);
        } else {
            // Unknown use — conservative
            markAllLive();
        }
    }
}

/// Analyze a node function to determine which fields of the struct it accesses.
std::set<unsigned> analyzeNodeFieldUsage(llvm::Function* nodeFunc,
                                         llvm::StructType* wrapperSty,
                                         llvm::StructType* elemSty) {
    std::set<unsigned> fields;

    for (auto& arg : nodeFunc->args()) {
        if (!arg.getType()->isPointerTy()) continue;

        // Check if this argument is used with the wrapper type
        bool usesWrapper = false;
        for (auto* user : arg.users()) {
            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
                if (gep->getSourceElementType() == wrapperSty) {
                    usesWrapper = true;
                    break;
                }
            }
        }

        if (usesWrapper) {
            std::unordered_set<llvm::Value*> visited;
            collectArrayFieldReads(&arg, wrapperSty, elemSty, fields, visited);
        }
    }

    return fields;
}

/// Get or declare an external function in the module.
llvm::FunctionCallee getOrDeclareFunc(llvm::Module& M, llvm::StringRef name, llvm::FunctionType* fty) {
    if (auto* existing = M.getFunction(name)) return existing;
    return M.getOrInsertFunction(name, fty);
}

// ============================================================================
// Step 4a: Inline + GEP Rewrite SoA transform (for auto-select variant)
// ============================================================================

/// Transform a pipeline function clone by:
///   1. Inlining all node function calls
///   2. Running mem2reg to clean up -O0 alloca chains
///   3. Heap-allocating SoA columns (aligned_alloc, 64B aligned)
///   4. Rewriting AoS GEPs to direct SoA column access
///   5. Adding a single scatter at entry (AoS→SoA) and gather at exit (SoA→AoS)
///      but NO per-node scatter/gather
///
/// This eliminates the per-node scatter/gather overhead that made the old
/// approach slower than AoS. After inlining, each field access in the loop
/// body directly hits the SoA column, giving true cache-line utilization.
// logicBlock / analysis / mapping / config remain in the signature so
// peer pipeline-rewrite entry points share one shape; this body only
// consumes `pipelineFunc` and `symbols`. Reintroducing one of the
// dropped sources is a body edit, not a call-site change.
int transformPipelineInlineRewrite(llvm::Function* pipelineFunc,
                                   [[maybe_unused]] const LogicBlockEntry& logicBlock,
                                   [[maybe_unused]] const PipelineAnalysis& analysis,
                                   [[maybe_unused]] const SymbolMapping& mapping,
                                   const SymbolTable& symbols,
                                   [[maybe_unused]] const DataLayoutConfig& config) {
    auto& ctx = pipelineFunc->getContext();
    int transformed = 0;

    // --- Step 0: Detect topo::array wrapper types in the module ---
    // At -O0, pointer params use operator[] calls (no GEPs on wrapper type).
    // Instead of following GEP uses, scan module struct types for topo::array
    // patterns and match against function parameter demangled names.
    struct ArrayParamInfo {
        unsigned argIndex;
        ArrayOfStructInfo info;
        const ClassSymbol* classSym;
    };
    std::vector<ArrayParamInfo> arrayParams;

    // First, find all topo::array wrapper types in the module
    std::vector<ArrayOfStructInfo> moduleArrayTypes;
    for (auto* sty : pipelineFunc->getParent()->getIdentifiedStructTypes()) {
        if (auto info = isFixedSizeArrayOfStruct(sty)) moduleArrayTypes.push_back(*info);
    }
    // For each pointer parameter, check if the function name suggests it's a
    // topo::array parameter, and match against known wrapper types by size.
    for (auto& arg : pipelineFunc->args()) {
        if (!arg.getType()->isPointerTy()) {
            continue;
        }

        // Try GEP-based detection first (works when inlined)
        auto gepInfo = getArrayParamInfo(&arg);
        if (gepInfo) {
            auto* cs = resolveStructFields(gepInfo->elementType, symbols);
            if (cs && cs->memberVars.size() == gepInfo->elementType->getNumElements()) {
                arrayParams.push_back({arg.getArgNo(), *gepInfo, cs});
                continue;
            }
        }

        // Fallback: match by demangled function name or first qualifying
        // topo::array type in the module.
        for (const auto& arrInfo : moduleArrayTypes) {
            auto* cs = resolveStructFields(arrInfo.elementType, symbols);
            // ClassSymbol is optional — if the element type is a plain C++
            // struct (not declared in .topo), we can still transform based
            // on IR struct layout alone. The topo::array<T,N> wrapper is
            // sufficient to know this is a Topo-managed container.
            if (cs && cs->memberVars.size() != arrInfo.elementType->getNumElements())
                continue; // ClassSymbol exists but field count mismatch — skip
            arrayParams.push_back({arg.getArgNo(), arrInfo, cs});
            break;
        }
    }

    if (arrayParams.empty()) return 0;

    // --- Collect all AoS GEPs that access struct fields ---
    // The pipeline expects inlining and SROA to have already run (via
    // TopoInlinePass + AlwaysInliner + SROA in PassPipeline), so GEP
    // patterns are one of:
    //   (a) wrapper[0][0][i][field] (srcTy = wrapperSty)
    //   (b) element[i, field] (srcTy = elemSty, 2 indices)
    for (const auto& ap : arrayParams) {
        auto* wrapperSty = ap.info.wrapperType;
        auto* elemSty = ap.info.elementType;
        uint64_t N = ap.info.arraySize;
        auto* arg = pipelineFunc->getArg(ap.argIndex);

        struct FieldGEP {
            llvm::GetElementPtrInst* gep;
            unsigned fieldIdx;
            llvm::Value* elementIdx;
        };
        std::vector<FieldGEP> fieldGEPs;
        std::set<unsigned> liveFields;

        // Collect all pointers derived from the array argument
        std::unordered_set<llvm::Value*> derivedPtrs;
        std::vector<llvm::Value*> worklist = {arg};
        while (!worklist.empty()) {
            auto* v = worklist.back();
            worklist.pop_back();
            if (!derivedPtrs.insert(v).second) continue;
            for (auto* user : v->users()) {
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user))
                    worklist.push_back(gep);
                else if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(user))
                    worklist.push_back(bc);
            }
        }

        for (auto& BB : *pipelineFunc) {
            for (auto& I : BB) {
                auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&I);
                if (!gep) continue;

                // Pattern (a): wrapper[0][0][i][field]
                if (gep->getSourceElementType() == wrapperSty && derivedPtrs.count(gep->getPointerOperand()) &&
                    gep->getNumOperands() >= 5) {
                    auto* fieldIdxVal = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(4));
                    if (!fieldIdxVal) continue;
                    unsigned fieldIdx = fieldIdxVal->getZExtValue();
                    if (fieldIdx >= elemSty->getNumElements()) continue;
                    fieldGEPs.push_back({gep, fieldIdx, gep->getOperand(3)});
                    liveFields.insert(fieldIdx);
                    continue;
                }

                // Pattern (b): 2-GEP chain after operator[] inlining:
                //   %elem = gep [N x Struct], ptr %data, 0, %i
                //   %field = gep Struct, ptr %elem, 0, fieldIdx
                // The element index %i is in the array GEP, not this one.
                if (gep->getSourceElementType() == elemSty && derivedPtrs.count(gep->getPointerOperand()) &&
                    gep->getNumIndices() == 2) {
                    auto* fieldIdxVal = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2));
                    if (!fieldIdxVal) continue;
                    unsigned fieldIdx = fieldIdxVal->getZExtValue();
                    if (fieldIdx >= elemSty->getNumElements()) continue;

                    // The first index is the struct pointer deref (always 0).
                    // The real element index is in the preceding array GEP.
                    llvm::Value* elementIdx = nullptr;
                    if (auto* ptrGEP = llvm::dyn_cast<llvm::GetElementPtrInst>(gep->getPointerOperand())) {
                        // Check for: gep [N x Struct], ptr, 0, %i
                        auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(ptrGEP->getSourceElementType());
                        if (arrTy && arrTy->getElementType() == elemSty && ptrGEP->getNumIndices() == 2) {
                            elementIdx = ptrGEP->getOperand(2);
                        }
                        // Also check: gep wrapperSty, ptr, 0, 0, %i
                        if (!elementIdx && ptrGEP->getSourceElementType() == wrapperSty &&
                            ptrGEP->getNumIndices() >= 3) {
                            elementIdx = ptrGEP->getOperand(3);
                        }
                    }
                    // Also handle call result (non-inlined operator[]):
                    // the return value is a pointer to a specific element,
                    // but we can't extract the index — skip these.
                    if (!elementIdx) continue;

                    fieldGEPs.push_back({gep, fieldIdx, elementIdx});
                    liveFields.insert(fieldIdx);
                    continue;
                }
            }
        }

        // Pattern (c): Direct loads/stores from array element GEPs = field 0.
        // When operator[] is inlined, field 0 (at struct offset 0) has no
        // separate struct field GEP. The load/store goes directly to the
        // element pointer: gep [N x Struct], ptr %data, 0, %i → load float.
        struct Field0Access {
            llvm::Instruction* inst; // load or store
            llvm::Value* elementIdx;
            llvm::GetElementPtrInst* elemGEP; // the array element GEP
        };
        std::vector<Field0Access> field0Accesses;

        for (auto& BB : *pipelineFunc) {
            for (auto& I : BB) {
                auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&I);
                if (!gep) continue;

                // Match: gep [N x Struct], ptr %derived, 0, %i
                auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(gep->getSourceElementType());
                if (!arrTy || arrTy->getElementType() != elemSty) continue;
                if (!derivedPtrs.count(gep->getPointerOperand())) continue;
                if (gep->getNumIndices() != 2) continue;

                auto* elementIdx = gep->getOperand(2);

                // Check if this GEP result is used directly for load/store
                // (not through a further struct field GEP).
                for (auto* user : gep->users()) {
                    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user)) {
                        if (load->getType()->isFloatTy() || load->getType()->isDoubleTy() ||
                            load->getType()->isIntegerTy()) {
                            field0Accesses.push_back({load, elementIdx, gep});
                        }
                    } else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user)) {
                        if (store->getPointerOperand() == gep) {
                            field0Accesses.push_back({store, elementIdx, gep});
                        }
                    }
                }
            }
        }
        if (!field0Accesses.empty()) liveFields.insert(0);

        if (fieldGEPs.empty() && field0Accesses.empty()) continue;

        // --- Step 4: Allocate SoA columns (heap, 64B aligned) ---
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::getUnqual(ctx);

        auto& entryBB = pipelineFunc->getEntryBlock();
        llvm::IRBuilder<> builder(&entryBB, entryBB.getFirstInsertionPt());

        auto* sizeTy = builder.getInt64Ty();
        auto* allocFTy = llvm::FunctionType::get(i8PtrTy, {sizeTy, sizeTy}, false);
        auto alignedAllocFn = getOrDeclareFunc(*pipelineFunc->getParent(), "aligned_alloc", allocFTy);
        auto* freeFTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {i8PtrTy}, false);
        auto freeFn = getOrDeclareFunc(*pipelineFunc->getParent(), "free", freeFTy);

        struct ColumnInfo {
            llvm::Value* ptr;
            llvm::ArrayType* arrTy;
        };
        std::map<unsigned, ColumnInfo> columns;
        auto& DL = pipelineFunc->getParent()->getDataLayout();

        for (unsigned fieldIdx : liveFields) {
            auto* fieldTy = elemSty->getElementType(fieldIdx);
            auto* arrTy = llvm::ArrayType::get(fieldTy, N);
            uint64_t colSize = DL.getTypeAllocSize(arrTy);
            auto* sizeVal = llvm::ConstantInt::get(i64Ty, colSize);
            auto* alignVal = llvm::ConstantInt::get(i64Ty, 64);
            auto* rawPtr =
                builder.CreateCall(alignedAllocFn, {alignVal, sizeVal}, "soa.col." + std::to_string(fieldIdx));
            columns[fieldIdx] = {rawPtr, arrTy};
        }

        // --- Step 5: Emit entry scatter: AoS → SoA columns ---
        // Find insertion point after column allocations
        auto* scatterBody = llvm::BasicBlock::Create(ctx, "soa.scatter", pipelineFunc);
        auto* scatterExit = llvm::BasicBlock::Create(ctx, "soa.scatter.exit", pipelineFunc);

        // Move original code after allocations into a new block
        auto* origBody = llvm::BasicBlock::Create(ctx, "pipeline.body", pipelineFunc);

        llvm::Instruction* splitPoint = nullptr;
        for (auto& I : entryBB) {
            if (llvm::isa<llvm::AllocaInst>(&I)) continue;
            if (auto* ci = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = ci->getCalledFunction()) {
                    if (callee->getName() == "aligned_alloc") continue;
                }
            }
            splitPoint = &I;
            break;
        }
        if (splitPoint) {
            origBody->splice(origBody->begin(), &entryBB, splitPoint->getIterator(), entryBB.end());
            // Update phi nodes in successor blocks: replace %entry → %pipeline.body
            auto* term = origBody->getTerminator();
            if (term) {
                for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
                    auto* succ = term->getSuccessor(i);
                    for (auto& phi : succ->phis())
                        phi.replaceIncomingBlockWith(&entryBB, origBody);
                }
            }
        }

        builder.SetInsertPoint(&entryBB);
        builder.CreateBr(scatterBody);

        // Scatter loop
        builder.SetInsertPoint(scatterBody);
        auto* sPhi = builder.CreatePHI(i64Ty, 2, "soa.si");
        sPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), &entryBB);

        for (auto& [fieldIdx, colInfo] : columns) {
            auto* fieldTy = elemSty->getElementType(fieldIdx);
            auto* srcGEP = builder.CreateGEP(wrapperSty,
                                             arg,
                                             {llvm::ConstantInt::get(i32Ty, 0),
                                              llvm::ConstantInt::get(i32Ty, 0),
                                              sPhi,
                                              llvm::ConstantInt::get(i32Ty, fieldIdx)},
                                             "soa.scatter.src");
            auto* val = builder.CreateLoad(fieldTy, srcGEP, "soa.scatter.val");
            auto* dstGEP = builder.CreateGEP(
                colInfo.arrTy, colInfo.ptr, {llvm::ConstantInt::get(i64Ty, 0), sPhi}, "soa.scatter.dst");
            builder.CreateStore(val, dstGEP);
        }

        auto* sNext = builder.CreateAdd(sPhi, llvm::ConstantInt::get(i64Ty, 1));
        sPhi->addIncoming(sNext, scatterBody);
        auto* sCond = builder.CreateICmpULT(sNext, llvm::ConstantInt::get(i64Ty, N));
        builder.CreateCondBr(sCond, scatterBody, scatterExit);

        builder.SetInsertPoint(scatterExit);
        builder.CreateBr(origBody);

        // --- Step 6: Rewrite AoS GEPs → SoA column GEPs ---
        for (auto& fg : fieldGEPs) {
            auto& colInfo = columns[fg.fieldIdx];
            auto* fieldTy = elemSty->getElementType(fg.fieldIdx);

            llvm::IRBuilder<> gepBuilder(fg.gep);
            auto* newGEP = gepBuilder.CreateGEP(colInfo.arrTy,
                                                colInfo.ptr,
                                                {llvm::ConstantInt::get(i64Ty, 0), fg.elementIdx},
                                                "soa.col." + std::to_string(fg.fieldIdx));

            fg.gep->replaceAllUsesWith(newGEP);
            fg.gep->eraseFromParent();
            (void)fieldTy;
        }

        // --- Step 6b: Rewrite field 0 direct accesses → SoA column 0 ---
        if (!field0Accesses.empty() && columns.count(0)) {
            auto& col0 = columns[0];
            for (auto& f0 : field0Accesses) {
                llvm::IRBuilder<> b(f0.inst);
                auto* newGEP =
                    b.CreateGEP(col0.arrTy, col0.ptr, {llvm::ConstantInt::get(i64Ty, 0), f0.elementIdx}, "soa.col.0");

                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(f0.inst)) {
                    auto* newLoad = b.CreateLoad(load->getType(), newGEP, "soa.f0.val");
                    load->replaceAllUsesWith(newLoad);
                    load->eraseFromParent();
                } else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(f0.inst)) {
                    b.CreateStore(store->getValueOperand(), newGEP);
                    store->eraseFromParent();
                }
            }
        }

        // --- Step 7: Emit exit gather before each return: SoA → AoS ---
        std::vector<llvm::ReturnInst*> returns;
        for (auto& BB : *pipelineFunc) {
            if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) returns.push_back(ret);
        }

        for (auto* ret : returns) {
            auto* retBlock = ret->getParent();
            auto* gatherBody = llvm::BasicBlock::Create(ctx, "soa.gather", pipelineFunc);
            auto* gatherExit = llvm::BasicBlock::Create(ctx, "soa.gather.exit", pipelineFunc);
            auto* newRetBlock = retBlock->splitBasicBlock(ret->getIterator(), "soa.ret");
            retBlock->getTerminator()->eraseFromParent();

            builder.SetInsertPoint(retBlock);
            builder.CreateBr(gatherBody);

            builder.SetInsertPoint(gatherBody);
            auto* gPhi = builder.CreatePHI(i64Ty, 2, "soa.gi");
            gPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), retBlock);

            for (auto& [fieldIdx, colInfo] : columns) {
                auto* fieldTy = elemSty->getElementType(fieldIdx);
                auto* srcGEP = builder.CreateGEP(
                    colInfo.arrTy, colInfo.ptr, {llvm::ConstantInt::get(i64Ty, 0), gPhi}, "soa.gather.src");
                auto* val = builder.CreateLoad(fieldTy, srcGEP, "soa.gather.val");
                auto* dstGEP = builder.CreateGEP(wrapperSty,
                                                 arg,
                                                 {llvm::ConstantInt::get(i32Ty, 0),
                                                  llvm::ConstantInt::get(i32Ty, 0),
                                                  gPhi,
                                                  llvm::ConstantInt::get(i32Ty, fieldIdx)},
                                                 "soa.gather.dst");
                builder.CreateStore(val, dstGEP);
            }

            auto* gNext = builder.CreateAdd(gPhi, llvm::ConstantInt::get(i64Ty, 1));
            gPhi->addIncoming(gNext, gatherBody);
            auto* gCond = builder.CreateICmpULT(gNext, llvm::ConstantInt::get(i64Ty, N));
            builder.CreateCondBr(gCond, gatherBody, gatherExit);

            // Free columns before return
            builder.SetInsertPoint(gatherExit);
            for (auto& [freeIdx, freeInfo] : columns)
                builder.CreateCall(freeFn, {freeInfo.ptr});
            builder.CreateBr(newRetBlock);
        }

        ++transformed;
    }

    return transformed;
}

// ============================================================================
// Step 4b: Scatter/Gather SoA transform (legacy)
// ============================================================================

/// Transform a single pipeline function's array parameters from AoS to SoA.
/// When useHeapAllocation is true, SoA columns are heap-allocated via
/// aligned_alloc (avoids L1 cache pressure from large stack arrays) and freed
/// before each return instruction.
/// Returns the number of arrays transformed.
int transformPipeline(llvm::Function* pipelineFunc,
                      const LogicBlockEntry& logicBlock,
                      const PipelineAnalysis& analysis,
                      const SymbolMapping& mapping,
                      const SymbolTable& symbols,
                      [[maybe_unused]] const DataLayoutConfig& config,
                      bool useHeapAllocation = false) {
    auto& ctx = pipelineFunc->getContext();
    int transformed = 0;

    // Find array parameters that qualify for transformation
    struct ArrayParamInfo {
        unsigned argIndex;
        ArrayOfStructInfo info;
        const ClassSymbol* classSym;
    };
    std::vector<ArrayParamInfo> arrayParams;

    for (auto& arg : pipelineFunc->args()) {
        auto info = getArrayParamInfo(&arg);
        if (!info) continue;

        auto* classSym = resolveStructFields(info->elementType, symbols);
        if (!classSym) continue;

        // Verify field count matches between IR and .topo declaration
        if (classSym->memberVars.size() != info->elementType->getNumElements()) continue;

        arrayParams.push_back({arg.getArgNo(), *info, classSym});
    }

    if (arrayParams.empty()) return 0;

    // Collect field usage per node function in the pipeline
    // nodeName -> set of field indices accessed
    std::unordered_map<std::string, std::set<unsigned>> nodeFieldUsage;

    // Also collect the union of all fields accessed by any node
    std::set<unsigned> allLiveFields;

    for (const auto& [nodeName, stage] : analysis.stages) {
        // Resolve node to qualified name
        std::string qualifiedCallee;
        for (const auto& calledFunc : logicBlock.calledFunctions) {
            auto pos = calledFunc.rfind("::");
            std::string simpleName = (pos != std::string::npos) ? calledFunc.substr(pos + 2) : calledFunc;
            if (simpleName == nodeName) {
                qualifiedCallee = calledFunc;
                break;
            }
        }

        if (qualifiedCallee.empty()) qualifiedCallee = nodeName;

        auto it = mapping.matched.find(qualifiedCallee);
        if (it == mapping.matched.end() || !it->second) continue;

        auto* nodeFunc = it->second;
        if (nodeFunc->isDeclaration()) continue;

        for (const auto& ap : arrayParams) {
            auto fields = analyzeNodeFieldUsage(nodeFunc, ap.info.wrapperType, ap.info.elementType);
            nodeFieldUsage[nodeName].insert(fields.begin(), fields.end());
            allLiveFields.insert(fields.begin(), fields.end());
        }
    }

    if (allLiveFields.empty()) return 0;

    // Now transform: for each qualifying array parameter, insert SoA
    // scatter/gather around the pipeline body.
    for (const auto& ap : arrayParams) {
        auto* wrapperSty = ap.info.wrapperType;
        auto* elemSty = ap.info.elementType;
        uint64_t N = ap.info.arraySize;
        auto* arg = pipelineFunc->getArg(ap.argIndex);

        // Find the entry block's first insertion point
        auto& entryBB = pipelineFunc->getEntryBlock();
        llvm::IRBuilder<> builder(&entryBB, entryBB.getFirstInsertionPt());

        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);

        // Create SoA column storage for each live field (align 64).
        // Stack mode: alloca (original behavior).
        // Heap mode: aligned_alloc — avoids L1 cache pressure for large arrays.
        struct ColumnInfo {
            llvm::Value* ptr;
            llvm::ArrayType* arrTy;
        };
        std::map<unsigned, ColumnInfo> columnPtrs;
        auto* i8PtrTy = llvm::PointerType::getUnqual(ctx);
        llvm::FunctionCallee alignedAllocFn, freeFn;
        if (useHeapAllocation) {
            auto* sizeTy = builder.getInt64Ty();
            auto* allocFTy = llvm::FunctionType::get(i8PtrTy, {sizeTy, sizeTy}, false);
            alignedAllocFn = getOrDeclareFunc(*pipelineFunc->getParent(), "aligned_alloc", allocFTy);
            auto* freeFTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {i8PtrTy}, false);
            freeFn = getOrDeclareFunc(*pipelineFunc->getParent(), "free", freeFTy);
        }

        for (unsigned fieldIdx : allLiveFields) {
            auto* fieldTy = elemSty->getElementType(fieldIdx);
            auto* arrTy = llvm::ArrayType::get(fieldTy, N);
            if (useHeapAllocation) {
                auto& DL = pipelineFunc->getParent()->getDataLayout();
                uint64_t colSize = DL.getTypeAllocSize(arrTy);
                auto* sizeVal = llvm::ConstantInt::get(i64Ty, colSize);
                auto* alignVal = llvm::ConstantInt::get(i64Ty, 64);
                auto* rawPtr = builder.CreateCall(alignedAllocFn, {alignVal, sizeVal}, "soa.col.raw");
                columnPtrs[fieldIdx] = {rawPtr, arrTy};
            } else {
                auto* alloca = builder.CreateAlloca(arrTy, nullptr);
                alloca->setAlignment(llvm::Align(64));
                columnPtrs[fieldIdx] = {alloca, arrTy};
            }
        }

        // Emit scatter loop: AoS → SoA columns
        // for (i = 0; i < N; ++i) { col_f[i] = arr.data_[i].field_f; }
        auto* scatterPreheader = &entryBB;
        auto* scatterBody = llvm::BasicBlock::Create(ctx, "soa.scatter", pipelineFunc);
        auto* scatterExit = llvm::BasicBlock::Create(ctx, "soa.scatter.exit", pipelineFunc);

        // Move all original instructions (after allocas) into a new block
        auto* origBody = llvm::BasicBlock::Create(ctx, "pipeline.body", pipelineFunc);

        // Find the split point: after all our new allocas / aligned_alloc calls
        llvm::Instruction* splitPoint = nullptr;
        for (auto& I : entryBB) {
            if (llvm::isa<llvm::AllocaInst>(&I)) continue;
            if (auto* ci = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = ci->getCalledFunction()) {
                    if (callee->getName() == "aligned_alloc") continue;
                }
            }
            splitPoint = &I;
            break;
        }

        if (splitPoint) {
            origBody->splice(origBody->begin(), &entryBB, splitPoint->getIterator(), entryBB.end());
        }

        // Branch from preheader to scatter loop
        builder.SetInsertPoint(scatterPreheader);
        builder.CreateBr(scatterBody);

        // Scatter loop body
        builder.SetInsertPoint(scatterBody);
        auto* phi = builder.CreatePHI(i64Ty, 2, "soa.i");
        phi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), scatterPreheader);

        for (auto& [fieldIdx, colInfo] : columnPtrs) {
            // Load: arr.data_[i].field_f
            auto* elemGEP = builder.CreateGEP(wrapperSty,
                                              arg,
                                              {llvm::ConstantInt::get(i32Ty, 0),
                                               llvm::ConstantInt::get(i32Ty, 0),
                                               phi,
                                               llvm::ConstantInt::get(i32Ty, fieldIdx)},
                                              "soa.src");
            auto* fieldTy = elemSty->getElementType(fieldIdx);
            auto* val = builder.CreateLoad(fieldTy, elemGEP, "soa.val");

            // Store: col_f[i] = val
            auto* dstGEP =
                builder.CreateGEP(colInfo.arrTy, colInfo.ptr, {llvm::ConstantInt::get(i64Ty, 0), phi}, "soa.dst");
            builder.CreateStore(val, dstGEP);
        }

        auto* nextI = builder.CreateAdd(phi, llvm::ConstantInt::get(i64Ty, 1), "soa.next");
        phi->addIncoming(nextI, scatterBody);
        auto* cond = builder.CreateICmpULT(nextI, llvm::ConstantInt::get(i64Ty, N), "soa.cmp");
        builder.CreateCondBr(cond, scatterBody, scatterExit);

        // Scatter exit → fall through to original pipeline body
        builder.SetInsertPoint(scatterExit);
        builder.CreateBr(origBody);

        // Now find all calls to node functions in the pipeline body and
        // insert per-node gather → call → scatter sequences.
        //
        // For each call to a node function that uses the array:
        //   1. Create a temporary AoS array
        //   2. Gather only the needed fields from SoA columns into temp
        //   3. Call the node with the temp
        //   4. Scatter back modified fields from temp into columns

        std::vector<llvm::CallInst*> nodeCalls;
        for (auto& BB : *pipelineFunc) {
            for (auto& I : BB) {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                    auto* callee = call->getCalledFunction();
                    if (callee && !callee->isDeclaration()) {
                        // Check if any arg is our array parameter
                        for (unsigned i = 0; i < call->arg_size(); ++i) {
                            if (call->getArgOperand(i) == arg) {
                                nodeCalls.push_back(call);
                                break;
                            }
                        }
                    }
                }
            }
        }

        for (auto* call : nodeCalls) {
            auto* callee = call->getCalledFunction();
            if (!callee) continue;

            // Find which node this is
            std::string nodeName;
            for (const auto& [nName, usage] : nodeFieldUsage) {
                std::string qualifiedCallee;
                for (const auto& cf : logicBlock.calledFunctions) {
                    auto pos = cf.rfind("::");
                    std::string sName = (pos != std::string::npos) ? cf.substr(pos + 2) : cf;
                    if (sName == nName) {
                        qualifiedCallee = cf;
                        break;
                    }
                }
                auto it = mapping.matched.find(qualifiedCallee);
                if (it != mapping.matched.end() && it->second == callee) {
                    nodeName = nName;
                    break;
                }
            }

            if (nodeName.empty()) continue;

            auto usageIt = nodeFieldUsage.find(nodeName);
            if (usageIt == nodeFieldUsage.end()) continue;

            const auto& neededFields = usageIt->second;
            if (neededFields.empty()) continue;

            // Insert gather/scatter around this call
            builder.SetInsertPoint(call);

            // Create temp AoS array
            auto* tempAlloca = builder.CreateAlloca(wrapperSty, nullptr, "soa.temp");

            // Gather loop: SoA columns → temp AoS (only needed fields)
            auto* gatherBody = llvm::BasicBlock::Create(ctx, "soa.gather." + nodeName, pipelineFunc);
            auto* gatherExit = llvm::BasicBlock::Create(ctx, "soa.gather.exit." + nodeName, pipelineFunc);
            auto* callBlock = llvm::BasicBlock::Create(ctx, "soa.call." + nodeName, pipelineFunc);

            // Split current block before the call
            auto* preCallBlock = call->getParent();
            auto* postCallBlock = preCallBlock->splitBasicBlock(call->getIterator(), "soa.post." + nodeName);

            // Remove the unconditional branch that splitBasicBlock inserted
            preCallBlock->getTerminator()->eraseFromParent();

            // Branch to gather loop
            builder.SetInsertPoint(preCallBlock);
            builder.CreateBr(gatherBody);

            // Gather loop body
            builder.SetInsertPoint(gatherBody);
            auto* gPhi = builder.CreatePHI(i64Ty, 2, "soa.gi");
            gPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), preCallBlock);

            for (unsigned fieldIdx : neededFields) {
                auto& colInfo = columnPtrs[fieldIdx];
                auto* fieldTy = elemSty->getElementType(fieldIdx);

                // Load from SoA column
                auto* srcGEP = builder.CreateGEP(
                    colInfo.arrTy, colInfo.ptr, {llvm::ConstantInt::get(i64Ty, 0), gPhi}, "soa.g.src");
                auto* val = builder.CreateLoad(fieldTy, srcGEP, "soa.g.val");

                // Store to temp AoS
                auto* dstGEP = builder.CreateGEP(wrapperSty,
                                                 tempAlloca,
                                                 {llvm::ConstantInt::get(i32Ty, 0),
                                                  llvm::ConstantInt::get(i32Ty, 0),
                                                  gPhi,
                                                  llvm::ConstantInt::get(i32Ty, fieldIdx)},
                                                 "soa.g.dst");
                builder.CreateStore(val, dstGEP);
            }

            auto* gNextI = builder.CreateAdd(gPhi, llvm::ConstantInt::get(i64Ty, 1), "soa.g.next");
            gPhi->addIncoming(gNextI, gatherBody);
            auto* gCond = builder.CreateICmpULT(gNextI, llvm::ConstantInt::get(i64Ty, N), "soa.g.cmp");
            builder.CreateCondBr(gCond, gatherBody, gatherExit);

            // Gather exit → call block
            builder.SetInsertPoint(gatherExit);
            builder.CreateBr(callBlock);

            // Call block: replace the array arg with temp, then scatter back
            builder.SetInsertPoint(callBlock);

            // Build new call with temp array instead of original
            std::vector<llvm::Value*> newArgs;
            for (unsigned i = 0; i < call->arg_size(); ++i) {
                if (call->getArgOperand(i) == arg)
                    newArgs.push_back(tempAlloca);
                else
                    newArgs.push_back(call->getArgOperand(i));
            }
            auto* newCall = builder.CreateCall(callee, newArgs);
            call->replaceAllUsesWith(newCall);

            // Scatter loop: temp AoS → SoA columns (only needed fields)
            auto* scatterNodeBody = llvm::BasicBlock::Create(ctx, "soa.scat." + nodeName, pipelineFunc);
            auto* scatterNodeExit = llvm::BasicBlock::Create(ctx, "soa.scat.exit." + nodeName, pipelineFunc);

            builder.CreateBr(scatterNodeBody);

            builder.SetInsertPoint(scatterNodeBody);
            auto* sPhi = builder.CreatePHI(i64Ty, 2, "soa.si");
            sPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), callBlock);

            for (unsigned fieldIdx : neededFields) {
                auto& colInfo = columnPtrs[fieldIdx];
                auto* fieldTy = elemSty->getElementType(fieldIdx);

                // Load from temp AoS
                auto* srcGEP = builder.CreateGEP(wrapperSty,
                                                 tempAlloca,
                                                 {llvm::ConstantInt::get(i32Ty, 0),
                                                  llvm::ConstantInt::get(i32Ty, 0),
                                                  sPhi,
                                                  llvm::ConstantInt::get(i32Ty, fieldIdx)},
                                                 "soa.s.src");
                auto* val = builder.CreateLoad(fieldTy, srcGEP, "soa.s.val");

                // Store to SoA column
                auto* dstGEP = builder.CreateGEP(
                    colInfo.arrTy, colInfo.ptr, {llvm::ConstantInt::get(i64Ty, 0), sPhi}, "soa.s.dst");
                builder.CreateStore(val, dstGEP);
            }

            auto* sNextI = builder.CreateAdd(sPhi, llvm::ConstantInt::get(i64Ty, 1), "soa.s.next");
            sPhi->addIncoming(sNextI, scatterNodeBody);
            auto* sCond = builder.CreateICmpULT(sNextI, llvm::ConstantInt::get(i64Ty, N), "soa.s.cmp");
            builder.CreateCondBr(sCond, scatterNodeBody, scatterNodeExit);

            // Connect scatter exit to the rest of the pipeline
            builder.SetInsertPoint(scatterNodeExit);
            builder.CreateBr(postCallBlock);

            // Remove the original call
            call->eraseFromParent();
        }

        // Emit final gather loop: SoA columns → original AoS array
        // Find the return block and insert gather before it
        for (auto& BB : *pipelineFunc) {
            auto* term = BB.getTerminator();
            if (!term || !llvm::isa<llvm::ReturnInst>(term)) continue;

            auto* gatherFinalBody = llvm::BasicBlock::Create(ctx, "soa.final.gather", pipelineFunc);
            auto* gatherFinalExit = llvm::BasicBlock::Create(ctx, "soa.final.exit", pipelineFunc);

            // Split before the return
            auto* retBlock = term->getParent();
            auto* newRetBlock = retBlock->splitBasicBlock(term->getIterator(), "soa.ret");
            retBlock->getTerminator()->eraseFromParent();

            builder.SetInsertPoint(retBlock);
            builder.CreateBr(gatherFinalBody);

            builder.SetInsertPoint(gatherFinalBody);
            auto* fPhi = builder.CreatePHI(i64Ty, 2, "soa.fi");
            fPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), retBlock);

            for (auto& [fieldIdx, colInfo] : columnPtrs) {
                auto* fieldTy = elemSty->getElementType(fieldIdx);

                auto* srcGEP = builder.CreateGEP(
                    colInfo.arrTy, colInfo.ptr, {llvm::ConstantInt::get(i64Ty, 0), fPhi}, "soa.f.src");
                auto* val = builder.CreateLoad(fieldTy, srcGEP, "soa.f.val");

                auto* dstGEP = builder.CreateGEP(wrapperSty,
                                                 arg,
                                                 {llvm::ConstantInt::get(i32Ty, 0),
                                                  llvm::ConstantInt::get(i32Ty, 0),
                                                  fPhi,
                                                  llvm::ConstantInt::get(i32Ty, fieldIdx)},
                                                 "soa.f.dst");
                builder.CreateStore(val, dstGEP);
            }

            auto* fNextI = builder.CreateAdd(fPhi, llvm::ConstantInt::get(i64Ty, 1), "soa.f.next");
            fPhi->addIncoming(fNextI, gatherFinalBody);
            auto* fCond = builder.CreateICmpULT(fNextI, llvm::ConstantInt::get(i64Ty, N), "soa.f.cmp");
            builder.CreateCondBr(fCond, gatherFinalBody, gatherFinalExit);

            builder.SetInsertPoint(gatherFinalExit);

            // In heap mode, free column memory before returning
            if (useHeapAllocation) {
                for (auto& [freeIdx, freeInfo] : columnPtrs) {
                    builder.CreateCall(freeFn, {freeInfo.ptr});
                }
            }

            builder.CreateBr(newRetBlock);

            break; // Only handle first return block
        }

        ++transformed;
    }

    return transformed;
}

} // anonymous namespace

// ============================================================================
// Entry point
// ============================================================================

int DataLayoutPass::run(llvm::Module& /*module*/,
                        const SymbolTable& symbols,
                        const SymbolMapping& mapping,
                        const DataLayoutConfig& config) {
    if (!config.isEnabled()) return 0;

    int totalTransformed = 0;

    for (const auto& [name, logicBlock] : symbols.logicBlocks()) {
        if (!logicBlock.isPipeline || !logicBlock.pipelineAnalysis) continue;

        const auto& analysis = *logicBlock.pipelineAnalysis;

        // Resolve pipeline function via mapping
        auto it = mapping.matched.find(logicBlock.qualifiedName);
        if (it == mapping.matched.end() || !it->second) continue;

        auto* pipelineFunc = it->second;
        if (pipelineFunc->isDeclaration()) continue;

        totalTransformed += transformPipeline(pipelineFunc, logicBlock, analysis, mapping, symbols, config);
    }

    return totalTransformed;
}

std::vector<LayoutVariantPair> DataLayoutPass::generateVariants([[maybe_unused]] llvm::Module& module,
                                                                const SymbolTable& symbols,
                                                                const SymbolMapping& mapping,
                                                                const DataLayoutConfig& config) {
    std::vector<LayoutVariantPair> result;
    if (!config.isEnabled()) return result;

    for (const auto& [name, logicBlock] : symbols.logicBlocks()) {
        if (!logicBlock.isPipeline || !logicBlock.pipelineAnalysis) continue;

        const auto& analysis = *logicBlock.pipelineAnalysis;

        auto it = mapping.matched.find(logicBlock.qualifiedName);
        if (it == mapping.matched.end() || !it->second) continue;

        auto* pipelineFunc = it->second;
        if (pipelineFunc->isDeclaration()) continue;

        // Clone the pipeline function to create the SoA variant
        llvm::ValueToValueMapTy vmap;
        auto* soaFunc = llvm::CloneFunction(pipelineFunc, vmap);
        soaFunc->setName(pipelineFunc->getName().str() + "__soa_variant");
        soaFunc->setLinkage(llvm::GlobalValue::InternalLinkage);

        // Apply inline + GEP rewrite SoA transform to the clone
        int transformed = transformPipelineInlineRewrite(soaFunc, logicBlock, analysis, mapping, symbols, config);

        if (transformed > 0) {
            result.push_back({pipelineFunc, soaFunc, name});
        } else {
            // No transformation applied — remove the clone
            soaFunc->eraseFromParent();
        }
    }

    return result;
}

// ============================================================================
// Module-wide SoA rewrite
// ============================================================================

/// Rewrite all topo::array<T,N> accesses from AoS to SoA layout globally.
/// Instead of per-call scatter/gather, this changes the physical memory layout
/// so all functions see consistent column-major access patterns.
///
/// Algorithm:
///   1. Find topo::array<T,N> wrapper types in the module
///   2. Create a SoA struct type: {[N x F0], [N x F1], ...}
///   3. For every function that accesses the array:
///      a. Inline operator[] to expose GEP patterns
///      b. Run mem2reg to clean up alloca chains
///      c. Rewrite field GEPs from AoS to SoA pattern
int DataLayoutPass::runForceSoA(llvm::Module& module, const DataLayoutConfig& /*config*/) {
    return runGlobalImpl(module);
}

int DataLayoutPass::runGlobal(llvm::Module& module, const DataLayoutConfig& config) {
    if (!config.isEnabled()) return 0;
    // Auto vs Force is decided by VariantBenchmark in PassPipeline.cpp; once
    // here, the Pass unconditionally applies the SoA transform on every
    // qualifying topo::array. The Pass itself does not make
    // cost/benefit judgments.
    return runGlobalImpl(module);
}

int DataLayoutPass::runGlobalImpl(llvm::Module& module) {
    // Step 1: Find all topo::array wrapper types
    struct GlobalArrayInfo {
        llvm::StructType* wrapperType;
        llvm::StructType* elementType;
        uint64_t arraySize;
        llvm::StructType* soaType; // created in step 2
    };
    std::vector<GlobalArrayInfo> arrayInfos;

    for (auto* sty : module.getIdentifiedStructTypes()) {
        if (auto info = isFixedSizeArrayOfStruct(sty)) {
            arrayInfos.push_back({info->wrapperType, info->elementType, info->arraySize, nullptr});
        }
    }
    // MSVC-ABI fallback: when the wrapper struct was elided (so the scan above
    // found nothing), recover (Elem, N) candidates from the mangled names that
    // still encode `topo::array<Elem, N>`. See recoverElidedArrayTypes.
    if (arrayInfos.empty()) {
        for (const auto& info : recoverElidedArrayTypes(module))
            arrayInfos.push_back({info.wrapperType, info.elementType, info.arraySize, nullptr});
    }
    if (arrayInfos.empty()) return 0;

    auto& ctx = module.getContext();

    // Step 2: Create SoA struct types
    for (auto& ai : arrayInfos) {
        std::vector<llvm::Type*> columns;
        for (unsigned i = 0; i < ai.elementType->getNumElements(); ++i) {
            auto* fieldTy = ai.elementType->getElementType(i);
            columns.push_back(llvm::ArrayType::get(fieldTy, ai.arraySize));
        }
        ai.soaType = llvm::StructType::create(ctx, columns, ai.wrapperType->getName().str() + ".soa");
    }

    int totalTransformed = 0;

    // Step 3: Process every function in the module
    for (auto& func : module) {
        if (func.isDeclaration()) continue;

        // Inlining and SROA have already run in PassPipeline (AlwaysInliner
        // + SROA after TopoInlinePass), so GEP patterns are exposed.

        // Check if this function accesses any topo::array type
        for (auto& ai : arrayInfos) {
            auto* wrapperSty = ai.wrapperType;
            auto* elemSty = ai.elementType;
            auto* soaSty = ai.soaType;
            (void)soaSty;

            auto* i32Ty = llvm::Type::getInt32Ty(ctx);
            auto* zero32 = llvm::ConstantInt::get(i32Ty, 0);

            // Collect ALL matching GEPs by type pattern (no pointer tracing needed)
            struct FieldGEP {
                llvm::GetElementPtrInst* gep;
                unsigned fieldIdx;
                llvm::Value* elementIdx;
                llvm::Value* basePtr;
            };
            std::vector<FieldGEP> fieldGEPs;

            struct Field0Access {
                llvm::Instruction* inst;
                llvm::Value* elementIdx;
                llvm::GetElementPtrInst* elemGEP;
                llvm::Value* basePtr;
            };
            std::vector<Field0Access> field0Accesses;

            for (auto& BB : func) {
                for (auto& I : BB) {
                    auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&I);
                    if (!gep) continue;

                    // Pattern (a): wrapper[0][0][i][field]
                    if (gep->getSourceElementType() == wrapperSty && gep->getNumOperands() >= 5) {
                        auto* fieldIdxVal = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(4));
                        if (!fieldIdxVal) continue;
                        unsigned fieldIdx = fieldIdxVal->getZExtValue();
                        if (fieldIdx >= elemSty->getNumElements()) continue;
                        auto* basePtr = gep->getPointerOperand();
                        fieldGEPs.push_back({gep, fieldIdx, gep->getOperand(3), basePtr});
                        continue;
                    }

                    // Pattern (b): 2-GEP chain: gep Struct, ptr %elem, 0, field
                    if (gep->getSourceElementType() == elemSty && gep->getNumIndices() == 2) {
                        auto* fieldIdxVal = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2));
                        if (!fieldIdxVal) continue;
                        unsigned fieldIdx = fieldIdxVal->getZExtValue();
                        if (fieldIdx >= elemSty->getNumElements()) continue;

                        llvm::Value* elementIdx = nullptr;
                        llvm::Value* basePtr = nullptr;
                        if (auto* ptrGEP = llvm::dyn_cast<llvm::GetElementPtrInst>(gep->getPointerOperand())) {
                            auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(ptrGEP->getSourceElementType());
                            if (arrTy && arrTy->getElementType() == elemSty && ptrGEP->getNumIndices() == 2) {
                                elementIdx = ptrGEP->getOperand(2);
                                basePtr = ptrGEP->getPointerOperand();
                                while (auto* bgep = llvm::dyn_cast<llvm::GetElementPtrInst>(basePtr))
                                    basePtr = bgep->getPointerOperand();
                            }
                            if (!elementIdx && ptrGEP->getSourceElementType() == wrapperSty &&
                                ptrGEP->getNumIndices() >= 3) {
                                elementIdx = ptrGEP->getOperand(3);
                                basePtr = ptrGEP->getPointerOperand();
                            }
                        }
                        if (!elementIdx) continue;
                        fieldGEPs.push_back({gep, fieldIdx, elementIdx, basePtr});
                        continue;
                    }

                    // Pattern (c): direct load/store on array element = field 0
                    auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(gep->getSourceElementType());
                    if (arrTy && arrTy->getElementType() == elemSty && gep->getNumIndices() == 2) {
                        auto* elementIdx = gep->getOperand(2);
                        auto* basePtr = gep->getPointerOperand();
                        while (auto* bgep = llvm::dyn_cast<llvm::GetElementPtrInst>(basePtr))
                            basePtr = bgep->getPointerOperand();

                        for (auto* user : gep->users()) {
                            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user)) {
                                if (load->getType()->isFloatTy() || load->getType()->isDoubleTy() ||
                                    load->getType()->isIntegerTy()) {
                                    field0Accesses.push_back({load, elementIdx, gep, basePtr});
                                }
                            } else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user)) {
                                if (store->getPointerOperand() == gep) {
                                    field0Accesses.push_back({store, elementIdx, gep, basePtr});
                                }
                            }
                        }
                    }
                }
            }

            // Pattern (d): field GEP on result of operator[] call
            // %ptr = call @topo::array::operator[](ptr %array, i64 %i)
            // %field = gep %struct.Particle, ptr %ptr, 0, fieldIdx
            // We know operator[] returns &data_[i], so basePtr = arg0, elementIdx = arg1
            for (auto& BB : func) {
                for (auto& I : BB) {
                    auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&I);
                    if (!gep) continue;
                    if (gep->getSourceElementType() != elemSty) continue;
                    if (gep->getNumIndices() != 2) continue;
                    auto* fieldIdxVal = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2));
                    if (!fieldIdxVal) continue;

                    // Check if pointer comes from operator[] call
                    auto* call = llvm::dyn_cast<llvm::CallInst>(gep->getPointerOperand());
                    if (!call) continue;
                    auto* callee = call->getCalledFunction();
                    if (!callee || !callee->getName().contains("topo5array") || !callee->getName().contains("ixEm"))
                        continue;

                    // operator[](this, index): arg0 = array ptr, arg1 = index
                    if (call->arg_size() < 2) continue;
                    auto* arrayPtr = call->getArgOperand(0);
                    auto* elementIdx = call->getArgOperand(1);

                    unsigned fieldIdx = fieldIdxVal->getZExtValue();
                    if (fieldIdx >= elemSty->getNumElements()) continue;

                    fieldGEPs.push_back({gep, fieldIdx, elementIdx, arrayPtr});
                }
            }

            if (fieldGEPs.empty() && field0Accesses.empty()) continue;

            // Step 3d: Rewrite GEPs to use SoA struct type
            // Old: wrapper[0][0][i][field] → addr of element[i].field
            // New: wrapper[0][field_col][i] → addr of column[field][i]
            for (auto& fg : fieldGEPs) {
                llvm::IRBuilder<> b(fg.gep);
                auto* newGEP = b.CreateGEP(soaSty,
                                           fg.basePtr,
                                           {zero32, llvm::ConstantInt::get(i32Ty, fg.fieldIdx), fg.elementIdx},
                                           "soa." + std::to_string(fg.fieldIdx));
                fg.gep->replaceAllUsesWith(newGEP);
                fg.gep->eraseFromParent();
            }

            // Rewrite field 0 direct accesses
            for (auto& f0 : field0Accesses) {
                llvm::IRBuilder<> b(f0.inst);
                auto* newGEP = b.CreateGEP(soaSty, f0.basePtr, {zero32, zero32, f0.elementIdx}, "soa.0");

                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(f0.inst)) {
                    auto* newLoad = b.CreateLoad(load->getType(), newGEP, "soa.f0");
                    load->replaceAllUsesWith(newLoad);
                    load->eraseFromParent();
                } else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(f0.inst)) {
                    b.CreateStore(store->getValueOperand(), newGEP);
                    store->eraseFromParent();
                }
            }

            ++totalTransformed;
        }
    }

    return totalTransformed;
}

} // namespace topo
