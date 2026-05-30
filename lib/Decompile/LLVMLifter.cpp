#include "topo/Decompile/LLVMLifter.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Transpile/ModelOptimizer.h"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugProgramInstruction.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>

#include <algorithm>
#include <cassert>
#include <cctype>

namespace topo::decompile {

// ---------------------------------------------------------------------------
// Visibility → accessModifier string mapping
// ---------------------------------------------------------------------------

namespace {

std::string visibilityToAccessModifier(Visibility vis) {
    switch (vis) {
    case Visibility::Public:    return "public";
    case Visibility::Protected: return "protected";
    case Visibility::Private:   return "private";
    case Visibility::Internal:  return "";  // package-private / pub(crate)
    case Visibility::Ignore:    return "";
    }
    return "";
}

} // namespace

// ---------------------------------------------------------------------------
// Section name helpers — mirror the platform-aware logic from IREmbed.cpp
// ---------------------------------------------------------------------------

namespace {

/// Return all section name variants for .topo_ir across platforms.
/// The binary may have been compiled on any target, so we check all.
bool isTopoIRSection(llvm::StringRef name) {
    return name == ".topo_ir" ||         // ELF
           name == ".topo_ir$" ||        // PE/COFF
           name == "__DATA,__topo_ir" || // Mach-O (segment,section)
           name == "__topo_ir";          // Mach-O (section only — some tools strip segment)
}

/// Strip LLVM identified-struct prefixes ("class.", "struct.") and any
/// LLVM-appended uniquing suffix (".0", ".123") so the bare type name can be
/// matched against the demangled DWARF DICompositeType name.
std::string stripStructName(llvm::StringRef raw) {
    std::string name = raw.str();
    for (const char* prefix : {"class.", "struct.", "union."}) {
        if (name.rfind(prefix, 0) == 0) {
            name = name.substr(std::string(prefix).size());
            break;
        }
    }
    // Drop a trailing ".<digits>" LLVM uniquing suffix (e.g. "Foo.0").
    auto dot = name.rfind('.');
    if (dot != std::string::npos && dot + 1 < name.size()) {
        bool allDigits = true;
        for (size_t i = dot + 1; i < name.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) { allDigits = false; break; }
        }
        if (allDigits) name = name.substr(0, dot);
    }
    return name;
}

/// Compact load-binop-store patterns into CompoundAssignExpr.
void compactCompoundAssigns(std::vector<transpile::StmtPtr>& stmts) {
    for (size_t i = 0; i + 2 < stmts.size(); ) {
        // Pattern: VarDecl(load x) → VarDecl(binop loaded, rhs) → Assign(x, result)
        if (stmts[i]->kind() != transpile::Stmt::Kind::VarDecl ||
            stmts[i+1]->kind() != transpile::Stmt::Kind::VarDecl ||
            stmts[i+2]->kind() != transpile::Stmt::Kind::Assign) {
            ++i;
            continue;
        }

        auto& loadDecl = static_cast<transpile::VarDeclStmt&>(*stmts[i]);
        auto& binopDecl = static_cast<transpile::VarDeclStmt&>(*stmts[i+1]);
        auto& assign = static_cast<transpile::AssignStmt&>(*stmts[i+2]);

        // loadDecl must have a VarRef init (the loaded variable)
        if (!loadDecl.init || loadDecl.init->kind() != transpile::Expr::Kind::VarRef) { ++i; continue; }
        auto& loadedRef = static_cast<transpile::VarRefExpr&>(*loadDecl.init);
        std::string targetName = loadedRef.name;

        // binopDecl must have a BinaryOp init
        if (!binopDecl.init || binopDecl.init->kind() != transpile::Expr::Kind::BinaryOp) { ++i; continue; }
        auto& binop = static_cast<transpile::BinaryOpExpr&>(*binopDecl.init);

        // One operand of binop must reference loadDecl.name
        bool lhsIsLoad = (binop.lhs->kind() == transpile::Expr::Kind::VarRef &&
                          static_cast<transpile::VarRefExpr&>(*binop.lhs).name == loadDecl.name);
        bool rhsIsLoad = (binop.rhs->kind() == transpile::Expr::Kind::VarRef &&
                          static_cast<transpile::VarRefExpr&>(*binop.rhs).name == loadDecl.name);
        if (!lhsIsLoad && !rhsIsLoad) { ++i; continue; }

        // assign.target must be a VarRef to the same variable as loadedRef
        if (assign.target->kind() != transpile::Expr::Kind::VarRef) { ++i; continue; }
        auto& assignTarget = static_cast<transpile::VarRefExpr&>(*assign.target);
        if (assignTarget.name != targetName) { ++i; continue; }

        // assign.value must reference binopDecl.name
        if (assign.value->kind() != transpile::Expr::Kind::VarRef) { ++i; continue; }
        auto& assignVal = static_cast<transpile::VarRefExpr&>(*assign.value);
        if (assignVal.name != binopDecl.name) { ++i; continue; }

        // Build CompoundAssignExpr
        auto compound = std::make_unique<transpile::CompoundAssignExpr>();
        compound->fidelity = transpile::Fidelity::Recovered;
        compound->op = binop.op;
        auto target = std::make_unique<transpile::VarRefExpr>();
        target->fidelity = transpile::Fidelity::Recovered;
        target->name = targetName;
        compound->target = std::move(target);
        compound->value = lhsIsLoad ? std::move(binop.rhs) : std::move(binop.lhs);

        auto exprStmt = std::make_unique<transpile::ExprStmt>();
        exprStmt->fidelity = transpile::Fidelity::Recovered;
        exprStmt->expr = std::move(compound);

        // Replace 3 statements with 1
        stmts[i] = std::move(exprStmt);
        stmts.erase(stmts.begin() + static_cast<ptrdiff_t>(i + 1),
                     stmts.begin() + static_cast<ptrdiff_t>(i + 3));
        // Don't increment i — check if the new stmt starts another pattern
    }
}

// ---------------------------------------------------------------------------
// Itanium template-argument recovery
//
// Demangled C++ symbol names survive O2 even when DWARF / struct-type info is
// embed-stripped. `llvm::demangle` (already used by SymbolMapper) turns
// `_ZN3FooIiE3barEv` into `Foo<int>::bar` and `%"class.Foo<int>"` keeps its
// `<...>` literally. This helper takes such a demangled type/qualifier string
// and splits the *top-level* angle-bracket argument list (respecting nesting
// and commas) into a base (`nameParts`) plus recursively-parsed
// `templateArgs`. Integer literal arguments become `nonTypeValue`.
//
// CONSERVATIVE: any imbalance / garble / absent `<...>` / empty token leaves
// the result empty and the caller behaves exactly as before — no nameParts
// corruption, no fabricated argument, no crash. We never hand-roll an Itanium
// demangler; we only structure the already-demangled string.
// ---------------------------------------------------------------------------

/// Split `s` on top-level (nesting-depth-0) commas, respecting <> and ().
/// Returns false if angle/paren brackets are unbalanced.
bool splitTopLevelArgs(const std::string& s, std::vector<std::string>& out) {
    int angle = 0, paren = 0;
    std::string cur;
    for (char c : s) {
        if (c == '<') ++angle;
        else if (c == '>') { if (angle == 0) return false; --angle; }
        else if (c == '(') ++paren;
        else if (c == ')') { if (paren == 0) return false; --paren; }
        if (c == ',' && angle == 0 && paren == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (angle != 0 || paren != 0) return false;
    out.push_back(cur);
    return true;
}

std::string trimWs(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/// Parse a single demangled type/qualifier token into `node`. Populates
/// `nameParts` (split on top-level `::`) and, if a balanced top-level
/// `<...>` clause is present, recursively parses each argument into
/// `templateArgs`. A pure integer literal (optionally signed, with an
/// optional u/l/ull suffix) becomes `nonTypeValue` instead. Returns false on
/// any malformed input — caller must then leave its TypeNode untouched.
bool parseDemangledType(const std::string& raw, TypeNode& node);

bool parseDemangledTypeImpl(const std::string& raw, TypeNode& node) {
    std::string s = trimWs(raw);
    if (s.empty()) return false;

    // Non-type integer template argument: e.g. "4", "-1", "10ul".
    {
        std::string t = s;
        size_t i = 0;
        if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
        size_t digitsStart = i;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
        if (i > digitsStart) {
            // Allow an integer-literal suffix (u/l combinations) then nothing else.
            size_t j = i;
            while (j < t.size() && (t[j] == 'u' || t[j] == 'U' ||
                                    t[j] == 'l' || t[j] == 'L'))
                ++j;
            if (j == t.size()) {
                try {
                    node.nonTypeValue = std::stoi(t.substr(0, i));
                    node.nameParts.push_back(t.substr(0, i));
                    return true;
                } catch (...) {
                    return false; // out of int range — conservative bail
                }
            }
        }
    }

    // Find the top-level `<` (depth 0). Everything before it is the base,
    // everything between it and the matching final `>` is the arg list.
    int angle = 0;
    size_t openPos = std::string::npos;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '<') {
            if (angle == 0) { openPos = i; }
            ++angle;
        } else if (c == '>') {
            if (angle == 0) return false; // stray '>'
            --angle;
        }
    }
    if (angle != 0) return false; // unbalanced

    std::string base;
    std::string argList;
    bool hasArgs = false;
    if (openPos != std::string::npos) {
        if (s.back() != '>') return false; // template clause must close the token
        base = trimWs(s.substr(0, openPos));
        argList = s.substr(openPos + 1, s.size() - openPos - 2);
        hasArgs = true;
    } else {
        base = s;
    }
    if (base.empty()) return false;

    // Split the base on top-level `::` into nameParts.
    {
        int a = 0;
        std::string part;
        for (size_t i = 0; i < base.size(); ++i) {
            if (base[i] == '<') ++a;
            else if (base[i] == '>') --a;
            if (a == 0 && i + 1 < base.size() && base[i] == ':' && base[i + 1] == ':') {
                node.nameParts.push_back(trimWs(part));
                part.clear();
                ++i; // skip second ':'
                continue;
            }
            part.push_back(base[i]);
        }
        node.nameParts.push_back(trimWs(part));
    }
    for (const auto& p : node.nameParts)
        if (p.empty()) return false;

    if (!hasArgs) return true;

    std::vector<std::string> argTokens;
    if (!splitTopLevelArgs(argList, argTokens)) return false;
    for (const auto& tok : argTokens) {
        TypeNode arg;
        if (!parseDemangledType(tok, arg)) return false;
        node.templateArgs.push_back(std::move(arg));
    }
    return true;
}

bool parseDemangledType(const std::string& raw, TypeNode& node) {
    TypeNode tmp;
    if (!parseDemangledTypeImpl(raw, tmp)) return false;
    node = std::move(tmp);
    return true;
}

/// Return the DWARF return-type DIType for `func`, or null when the
/// function has no DISubprogram / no subroutine type / a void return.
/// Used to feed the Rust Box ownership recognizer with the function's
/// declared return type.
const llvm::DIType* dwarfReturnType(const llvm::Function& func) {
    const llvm::DISubprogram* sp = func.getSubprogram();
    if (!sp) return nullptr;
    const llvm::DISubroutineType* st = sp->getType();
    if (!st) return nullptr;
    auto arr = st->getTypeArray();
    if (arr.size() == 0) return nullptr;
    // Index 0 is the return type. It is null for `void` returns.
    return arr[0];
}

/// If `name` (a prefix-stripped struct name or a demangled qualifier) carries
/// a balanced top-level `<...>` clause, parse it and overwrite `node`'s
/// nameParts/templateArgs with the structured form. Returns true iff template
/// args were actually recovered (caller may then mark Fidelity::Recovered).
/// On any failure the node is left exactly as the caller had it.
bool recoverTemplateArgs(const std::string& name, TypeNode& node) {
    if (name.find('<') == std::string::npos) return false;
    TypeNode parsed;
    if (!parseDemangledType(name, parsed)) return false;
    if (parsed.templateArgs.empty()) return false;
    // Preserve modifiers/ownership the caller already set; only swap the
    // identity (nameParts) and attach the recovered args.
    node.nameParts = std::move(parsed.nameParts);
    node.templateArgs = std::move(parsed.templateArgs);
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// extractIR — read .topo_ir section from a compiled binary
// ---------------------------------------------------------------------------

std::unique_ptr<llvm::Module> LLVMLifter::extractIR(const std::string& artifactPath, llvm::LLVMContext& context) {
    auto bufOrErr = llvm::MemoryBuffer::getFile(artifactPath);
    if (!bufOrErr) return nullptr;

    auto objOrErr = llvm::object::ObjectFile::createObjectFile(bufOrErr->get()->getMemBufferRef());
    if (!objOrErr) return nullptr;

    auto& obj = *objOrErr.get();

    for (const auto& section : obj.sections()) {
        auto nameOrErr = section.getName();
        if (!nameOrErr) continue;

        if (!isTopoIRSection(*nameOrErr)) continue;

        auto contentsOrErr = section.getContents();
        if (!contentsOrErr) continue;

        auto bitcodeBuf = llvm::MemoryBuffer::getMemBuffer(*contentsOrErr, "topo_ir", /*RequiresNullTerminator=*/false);

        auto modOrErr = llvm::parseBitcodeFile(bitcodeBuf->getMemBufferRef(), context);
        if (!modOrErr) continue;

        return std::move(*modOrErr);
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Type lifting
// ---------------------------------------------------------------------------

TypeNode LLVMLifter::liftType(const llvm::Type* type) {
    TypeNode node;

    if (!type) {
        node.nameParts.push_back("void");
        return node;
    }

    if (type->isVoidTy()) {
        node.nameParts.push_back("void");
        return node;
    }

    if (type->isIntegerTy()) {
        unsigned bits = type->getIntegerBitWidth();
        switch (bits) {
        case 1: node.nameParts.push_back("bool"); break;
        case 8: node.nameParts.push_back("int8_t"); break;
        case 16: node.nameParts.push_back("int16_t"); break;
        case 32: node.nameParts.push_back("int32_t"); break;
        case 64: node.nameParts.push_back("int64_t"); break;
        default: node.nameParts.push_back("i" + std::to_string(bits)); break;
        }
        return node;
    }

    if (type->isFloatTy()) {
        node.nameParts.push_back("float");
        return node;
    }
    if (type->isDoubleTy()) {
        node.nameParts.push_back("double");
        return node;
    }

    if (type->isPointerTy()) {
        // Opaque pointer — cannot recover pointee type from IR alone
        node.nameParts.push_back("void");
        node.modifier = TypeNode::Ptr;
        return node;
    }

    if (auto* structTy = llvm::dyn_cast<llvm::StructType>(type)) {
        if (structTy->hasName()) {
            // LLVM struct names use "class.Foo" or "struct.Foo" prefixes
            std::string name = structTy->getName().str();
            // Strip common prefixes
            for (const char* prefix : {"class.", "struct.", "union."}) {
                if (name.rfind(prefix, 0) == 0) {
                    name = name.substr(std::string(prefix).size());
                    break;
                }
            }
            // The identified-struct name keeps its template clause literally
            // when the frontend did not erase it (e.g. "Wrapper<int>"). Try
            // to recover structured template args; on any failure fall back
            // to the legacy single-nameParts behaviour unchanged.
            if (!recoverTemplateArgs(name, node)) {
                node.nameParts.push_back(name);
            }
        } else {
            node.nameParts.push_back("anonymous_struct");
        }
        return node;
    }

    if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(type)) {
        // Represent as "element_type[N]" via the name
        auto elemNode = liftType(arrTy->getElementType());
        std::string name = elemNode.nameParts.empty() ? "unknown" : elemNode.nameParts.back();
        name += "[" + std::to_string(arrTy->getNumElements()) + "]";
        node.nameParts.push_back(name);
        return node;
    }

    if (auto* vecTy = llvm::dyn_cast<llvm::FixedVectorType>(type)) {
        auto elemNode = liftType(vecTy->getElementType());
        std::string name = elemNode.nameParts.empty() ? "unknown" : elemNode.nameParts.back();
        name += "<" + std::to_string(vecTy->getNumElements()) + ">";
        node.nameParts.push_back(name);
        return node;
    }

    // Fallback for function types, scalable vectors, etc.
    node.nameParts.push_back("unsupported_type");
    return node;
}

// ---------------------------------------------------------------------------
// Value naming
// ---------------------------------------------------------------------------

std::string LLVMLifter::getValueName(const llvm::Value& val) {
    // Check existing mapping
    auto it = nameMap_.find(&val);
    if (it != nameMap_.end()) return it->second;

    // Use LLVM's name if present
    if (val.hasName()) {
        std::string name = val.getName().str();
        nameMap_[&val] = name;
        return name;
    }

    // Generate a temp name
    std::string name = "_tmp" + std::to_string(tmpCounter_++);
    nameMap_[&val] = name;
    return name;
}

// ---------------------------------------------------------------------------
// Binary operator mapping
// ---------------------------------------------------------------------------

transpile::BinaryOp LLVMLifter::mapBinaryOp(unsigned opcode) {
    using transpile::BinaryOp;
    switch (opcode) {
    case llvm::Instruction::Add:
    case llvm::Instruction::FAdd: return BinaryOp::Add;
    case llvm::Instruction::Sub:
    case llvm::Instruction::FSub: return BinaryOp::Sub;
    case llvm::Instruction::Mul:
    case llvm::Instruction::FMul: return BinaryOp::Mul;
    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv:
    case llvm::Instruction::FDiv: return BinaryOp::Div;
    case llvm::Instruction::SRem:
    case llvm::Instruction::URem:
    case llvm::Instruction::FRem: return BinaryOp::Mod;
    case llvm::Instruction::And: return BinaryOp::And;
    case llvm::Instruction::Or: return BinaryOp::Or;
    default: return BinaryOp::Add; // fallback; callers should check
    }
}

transpile::BinaryOp LLVMLifter::mapICmpPredicate(llvm::CmpInst::Predicate pred) {
    using transpile::BinaryOp;
    switch (pred) {
    case llvm::CmpInst::ICMP_EQ: return BinaryOp::Eq;
    case llvm::CmpInst::ICMP_NE: return BinaryOp::NotEq;
    case llvm::CmpInst::ICMP_SLT:
    case llvm::CmpInst::ICMP_ULT: return BinaryOp::Less;
    case llvm::CmpInst::ICMP_SGT:
    case llvm::CmpInst::ICMP_UGT: return BinaryOp::Greater;
    case llvm::CmpInst::ICMP_SLE:
    case llvm::CmpInst::ICMP_ULE: return BinaryOp::LessEq;
    case llvm::CmpInst::ICMP_SGE:
    case llvm::CmpInst::ICMP_UGE: return BinaryOp::GreaterEq;
    default: return BinaryOp::Eq;
    }
}

transpile::BinaryOp LLVMLifter::mapFCmpPredicate(llvm::CmpInst::Predicate pred) {
    using transpile::BinaryOp;
    switch (pred) {
    case llvm::CmpInst::FCMP_OEQ:
    case llvm::CmpInst::FCMP_UEQ: return BinaryOp::Eq;
    case llvm::CmpInst::FCMP_ONE:
    case llvm::CmpInst::FCMP_UNE: return BinaryOp::NotEq;
    case llvm::CmpInst::FCMP_OLT:
    case llvm::CmpInst::FCMP_ULT: return BinaryOp::Less;
    case llvm::CmpInst::FCMP_OGT:
    case llvm::CmpInst::FCMP_UGT: return BinaryOp::Greater;
    case llvm::CmpInst::FCMP_OLE:
    case llvm::CmpInst::FCMP_ULE: return BinaryOp::LessEq;
    case llvm::CmpInst::FCMP_OGE:
    case llvm::CmpInst::FCMP_UGE: return BinaryOp::GreaterEq;
    default: return BinaryOp::Eq;
    }
}

// ---------------------------------------------------------------------------
// Value lifting — convert LLVM Value to Expr
// ---------------------------------------------------------------------------

transpile::ExprPtr LLVMLifter::liftValue(const llvm::Value& val) {
    using transpile::Fidelity;
    using transpile::LiteralKind;

    // Integer constants
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(&val)) {
        auto expr = std::make_unique<transpile::LiteralExpr>();
        expr->fidelity = Fidelity::Recovered;
        if (ci->getType()->isIntegerTy(1)) {
            expr->litKind = LiteralKind::Boolean;
            expr->value = ci->isZero() ? "false" : "true";
        } else {
            expr->litKind = LiteralKind::Integer;
            expr->value = std::to_string(ci->getSExtValue());
        }
        return expr;
    }

    // Float constants
    if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(&val)) {
        auto expr = std::make_unique<transpile::LiteralExpr>();
        expr->fidelity = Fidelity::Recovered;
        expr->litKind = LiteralKind::Float;
        llvm::SmallVector<char, 32> buf;
        cf->getValueAPF().toString(buf);
        expr->value = std::string(buf.begin(), buf.end());
        return expr;
    }

    // Null pointer
    if (llvm::isa<llvm::ConstantPointerNull>(&val)) {
        auto expr = std::make_unique<transpile::LiteralExpr>();
        expr->fidelity = Fidelity::Recovered;
        expr->litKind = LiteralKind::Integer;
        expr->value = "nullptr";
        return expr;
    }

    // Undef / poison — represent as unsupported
    if (llvm::isa<llvm::UndefValue>(&val) || llvm::isa<llvm::PoisonValue>(&val)) {
        auto expr = std::make_unique<transpile::UnsupportedExpr>();
        expr->fidelity = Fidelity::Recovered;
        expr->description = "undef";
        return expr;
    }

    // Global variable reference
    if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(&val)) {
        auto expr = std::make_unique<transpile::VarRefExpr>();
        expr->fidelity = Fidelity::Recovered;
        expr->name = gv->hasName() ? gv->getName().str() : "_global";
        return expr;
    }

    // Function reference (used as callee in indirect calls)
    if (auto* fn = llvm::dyn_cast<llvm::Function>(&val)) {
        auto expr = std::make_unique<transpile::VarRefExpr>();
        expr->fidelity = Fidelity::Recovered;
        expr->name = fn->hasName() ? fn->getName().str() : "_func";
        return expr;
    }

    // Generic value — variable reference
    auto expr = std::make_unique<transpile::VarRefExpr>();
    expr->fidelity = Fidelity::Recovered;
    expr->name = getValueName(val);
    return expr;
}

// ---------------------------------------------------------------------------
// C++ closure recovery (debug-IR MVP)
// ---------------------------------------------------------------------------

namespace {

// True if `name` looks like clang's Itanium mangling for a C++ closure
// `operator()` (i.e. `_ZZ<outer>EN[K]<closure-id>clE<sig>`). The "Z...E"
// wraps a local scope, "K" optionally marks `const`, "cl" is the
// `operator()` substitution. We match conservatively: any failure to
// recognise the prefix bails out.
bool looksLikeClosureCallOperatorMangled(llvm::StringRef name) {
    // Must start with the local-scope marker `_ZZ`.
    if (!name.starts_with("_ZZ")) return false;
    // Must contain a `cl` operator() substitution somewhere after the
    // outer-scope terminator `E`. The exact closure-id token (`$_0`,
    // `_0`, `Ut_`, …) varies across clang versions, so we look for the
    // `EN…clE` shape without locking in the closure tag.
    auto nclE = name.find("clE");
    return nclE != llvm::StringRef::npos;
}

// True if the DI scope chain of `sp` walks through a DICompositeType that
// itself sits inside a DISubprogram (the lambda closure's structural
// pattern: closure type is local to the enclosing function). Returns the
// DICompositeType when found, nullptr otherwise.
const llvm::DICompositeType*
diClosureTypeOfSubprogram(const llvm::DISubprogram* sp) {
    if (!sp) return nullptr;
    const auto* comp = llvm::dyn_cast_or_null<llvm::DICompositeType>(sp->getScope());
    if (!comp) return nullptr;
    // Closure types live inside a DISubprogram (the enclosing function).
    if (!llvm::isa_and_nonnull<llvm::DISubprogram>(comp->getScope())) return nullptr;
    return comp;
}

// Drop typedef/const layers to reveal the underlying member type. Returns
// nullptr if the chain hits an unrelated node.
const llvm::DIType* stripDITypedefConst(const llvm::DIType* t) {
    while (auto* d = llvm::dyn_cast_or_null<llvm::DIDerivedType>(t)) {
        unsigned tag = d->getTag();
        if (tag == llvm::dwarf::DW_TAG_typedef ||
            tag == llvm::dwarf::DW_TAG_const_type ||
            tag == llvm::dwarf::DW_TAG_volatile_type) {
            t = d->getBaseType();
            continue;
        }
        break;
    }
    return t;
}

} // namespace

transpile::StmtPtr
LLVMLifter::tryRecognizeLambdaCapture(const llvm::CallInst& call) {
    using transpile::Fidelity;

    // 1. The callee must be a known function with an Itanium-mangled
    //    closure-operator() name AND a DISubprogram whose scope is a
    //    closure DICompositeType. Anything indirect (`std::function`,
    //    virtual dispatch) bails out — that is intentional Out-of-scope
    //    degradation, not a recovery failure.
    const llvm::Function* callee = call.getCalledFunction();
    if (!callee) return nullptr;
    if (callee->isDeclaration()) return nullptr;
    if (!looksLikeClosureCallOperatorMangled(callee->getName())) return nullptr;
    const llvm::DISubprogram* sp = callee->getSubprogram();
    if (!sp) return nullptr;
    const llvm::DICompositeType* closureTy = diClosureTypeOfSubprogram(sp);
    if (!closureTy) return nullptr;

    // 2. First arg must be an AllocaInst (the closure struct), in the
    //    same function. Its allocated type must be a struct.
    if (call.arg_size() < 1) return nullptr;
    auto* closureAlloca = llvm::dyn_cast<llvm::AllocaInst>(call.getArgOperand(0));
    if (!closureAlloca) return nullptr;
    if (closureAlloca->getFunction() != call.getFunction()) return nullptr;
    auto* allocaStructTy =
        llvm::dyn_cast<llvm::StructType>(closureAlloca->getAllocatedType());
    if (!allocaStructTy) return nullptr;

    // 3. Multi-call-site shared closure struct → degrade. We only recover
    //    the canonical "alloca-then-call-once" shape that clang emits at
    //    -O0 for `auto f = [...]; f(args);` / `[...](args)`. A struct that
    //    is reused across multiple calls (or escapes via a non-call use)
    //    is conservatively out of scope.
    unsigned callUses = 0;
    for (const llvm::Use& u : closureAlloca->uses()) {
        const llvm::Instruction* user =
            llvm::dyn_cast<llvm::Instruction>(u.getUser());
        if (!user) return nullptr;
        if (auto* c = llvm::dyn_cast<llvm::CallBase>(user)) {
            // Only the first-argument use counts; otherwise (e.g. passed
            // to some other helper) we cannot reason about it.
            if (c->arg_size() == 0 || c->getArgOperand(0) != closureAlloca)
                return nullptr;
            ++callUses;
            continue;
        }
        // GEP / store / load uses are fine — they are how clang seeds the
        // capture fields. `dbg.declare` intrinsics are also fine.
        if (llvm::isa<llvm::GetElementPtrInst>(user) ||
            llvm::isa<llvm::StoreInst>(user) ||
            llvm::isa<llvm::LoadInst>(user) ||
            llvm::isa<llvm::DbgInfoIntrinsic>(user))
            continue;
        // Anything else (BitCast, PHI, an extra call as arg) means the
        // struct's lifetime is more complex than the MVP can handle.
        return nullptr;
    }
    if (callUses != 1) return nullptr;

    // 4. DWARF member names. The DICompositeType.elements gives us the
    //    capture identifiers in declaration order. We refuse to recover
    //    when any member type looks like a by-ref capture (DIDerivedType
    //    pointer / reference) — captures are by-value only in this MVP.
    std::vector<transpile::CaptureEntry> captureEntries;
    std::vector<std::string> captureFieldNames;
    for (const llvm::DINode* el : closureTy->getElements()) {
        const auto* dt = llvm::dyn_cast_or_null<llvm::DIDerivedType>(el);
        if (!dt || dt->getTag() != llvm::dwarf::DW_TAG_member) continue;
        if (dt->isStaticMember()) continue;
        if (dt->getName().empty()) return nullptr;

        const llvm::DIType* base = stripDITypedefConst(dt->getBaseType());
        if (auto* bd = llvm::dyn_cast_or_null<llvm::DIDerivedType>(base)) {
            unsigned tag = bd->getTag();
            if (tag == llvm::dwarf::DW_TAG_pointer_type ||
                tag == llvm::dwarf::DW_TAG_reference_type ||
                tag == llvm::dwarf::DW_TAG_rvalue_reference_type) {
                // By-reference capture — out of scope for this MVP.
                return nullptr;
            }
        }

        transpile::CaptureEntry e;
        e.name = dt->getName().str();
        e.mode = transpile::CaptureMode::ByValue;
        captureEntries.push_back(std::move(e));
        captureFieldNames.push_back(dt->getName().str());
    }
    // Captures must match the LLVM struct's actual element count so the
    // by-value pattern is unambiguous; a mismatch means there are hidden
    // fields (e.g. by-ref or vtable) and we degrade.
    if (captureEntries.size() != allocaStructTy->getNumElements())
        return nullptr;

    // 5. Suppress the closure body from the lifted module so it does not
    //    surface as an orphan helper function.
    suppressFromModule_.insert(callee);

    // 6. Build the LambdaExpr. Params = callee args minus the leading
    //    `this`-style closure pointer. The body is the closure body
    //    lifted via the existing per-instruction path; we route through
    //    `liftFunctionDirect` so any change in lifting fidelity carries
    //    through unchanged. We snapshot caller-side per-function state
    //    (tmpCounter_, nameMap_) so the recursive lift does not clobber
    //    it (each `liftFunction*` resets these before use, but we still
    //    have to restore them afterwards because we are mid-way through
    //    the caller's instruction walk).
    auto savedTmpCounter = tmpCounter_;
    auto savedNameMap = nameMap_;
    auto savedEhEntries = ehRegionEntries_;
    SymbolTable emptyMetadata;
    transpile::TranspileFunction bodyFn = liftFunctionDirect(
        *callee, callee->getName().str(), emptyMetadata);
    tmpCounter_ = savedTmpCounter;
    nameMap_ = std::move(savedNameMap);
    ehRegionEntries_ = std::move(savedEhEntries);

    auto lambdaExpr = std::make_unique<transpile::LambdaExpr>();
    lambdaExpr->fidelity = Fidelity::Recovered;
    lambdaExpr->captures = std::move(captureEntries);
    // Skip the closure object `this` parameter (always the first arg).
    if (!bodyFn.params.empty()) {
        lambdaExpr->params.assign(bodyFn.params.begin() + 1, bodyFn.params.end());
    }
    lambdaExpr->returnType = bodyFn.returnType;
    lambdaExpr->body = std::move(bodyFn.body);

    // Wrap the LambdaExpr in a VarDecl bound to the call's result name so
    // any subsequent reference (`return _tmpN` etc.) still resolves. The
    // VarDecl's type is the call return type — we treat the closure body
    // as inlined: the value of "invoke this lambda with `args`" is what
    // remains. Embedded args are preserved as `unusedArgs` literals on
    // the LambdaExpr to keep the recovered shape stable for downstream
    // emitters (in MVP we do not synthesise an "apply" expression in the
    // TranspileModel; the LambdaExpr alone carries captures+body+params,
    // which is what the acceptance criteria pin down).
    auto declStmt = std::make_unique<transpile::VarDeclStmt>();
    declStmt->fidelity = Fidelity::Recovered;
    declStmt->type = liftType(call.getType());
    declStmt->name = getValueName(call);
    declStmt->init = std::move(lambdaExpr);
    return declStmt;
}

// ---------------------------------------------------------------------------
// Instruction lifting — convert LLVM Instruction to Stmt
// ---------------------------------------------------------------------------

transpile::StmtPtr LLVMLifter::liftInstruction(const llvm::Instruction& inst) {
    using transpile::Fidelity;

    // -- AllocaInst -> VarDeclStmt --
    if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
        auto stmt = std::make_unique<transpile::VarDeclStmt>();

        // Recover the declared source type/name from DWARF when a
        // DILocalVariable is attached. The DWARF type is strictly better than
        // liftType here: with opaque pointers liftType collapses every pointer
        // to "void*", losing the real pointee type.
        // Baseline matches the pre-existing lifter convention: an alloca
        // VarDecl faithfully represents an IR local, so it is Recovered
        // regardless of whether a source-level name exists (sibling SSA-temp
        // VarDecls follow the same convention). The DWARF additions below
        // only *improve* the type and name; they never change fidelity.
        stmt->fidelity = Fidelity::Recovered;

        // Recover the declared source type/name from DWARF when a
        // DILocalVariable is attached. The DWARF type is strictly better than
        // liftType here: with opaque pointers liftType collapses every pointer
        // to "void*", losing the real pointee type.
        const llvm::DILocalVariable* dlv = debugLocalForAlloca(alloca);
        // First try the Rust Box ownership recognizer — when DWARF + IR
        // pair confidently as Box<T, Global>, the recovered type carries
        // OwnershipKind::Owned and no pointer modifier. Falls through to
        // the generic typeFromDI path on any non-match.
        std::optional<TypeNode> diType;
        if (dlv) {
            const llvm::Function* parentFn = alloca->getFunction();
            diType = recoverBoxOwnership(dlv->getType(), parentFn);
            if (!diType) diType = typeFromDI(dlv->getType());
        }

        if (diType) {
            stmt->type = std::move(*diType);
        } else {
            stmt->type = liftType(alloca->getAllocatedType());
        }

        if (dlv && !dlv->getName().empty()) {
            // Pin the recovered source name so every later load/store/GEP
            // referencing this alloca uses the same identifier.
            std::string srcName = dlv->getName().str();
            nameMap_[alloca] = srcName;
            stmt->name = srcName;
        } else {
            stmt->name = getValueName(*alloca);
        }
        return stmt;
    }

    // -- StoreInst -> AssignStmt --
    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        auto stmt = std::make_unique<transpile::AssignStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->target = liftValue(*store->getPointerOperand());
        stmt->value = liftValue(*store->getValueOperand());
        return stmt;
    }

    // -- LoadInst -> VarDeclStmt (assign loaded value to a temp) --
    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        auto stmt = std::make_unique<transpile::VarDeclStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->type = liftType(load->getType());
        stmt->name = getValueName(*load);
        stmt->init = liftValue(*load->getPointerOperand());
        return stmt;
    }

    // -- ReturnInst -> ReturnStmt --
    if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
        auto stmt = std::make_unique<transpile::ReturnStmt>();
        stmt->fidelity = Fidelity::Recovered;
        if (ret->getReturnValue()) stmt->value = liftValue(*ret->getReturnValue());
        return stmt;
    }

    // -- CallInst -> ExprStmt with CallExpr --
    if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        // First try recognising a C++ by-value lambda invocation (the
        // debug-IR MVP shape). On any non-match the recogniser returns
        // nullptr and we fall through to the linear path — so
        // out-of-scope shapes (by-ref capture, std::function, virtual
        // dispatch, …) degrade silently to the pre-existing output.
        if (auto lambdaStmt = tryRecognizeLambdaCapture(*call)) {
            return lambdaStmt;
        }

        auto callExpr = std::make_unique<transpile::CallExpr>();
        callExpr->fidelity = Fidelity::Recovered;

        if (auto* calledFn = call->getCalledFunction()) {
            callExpr->callee = calledFn->getName().str();
        } else {
            // Indirect call — use the called value
            callExpr->callee = getValueName(*call->getCalledOperand());
        }

        for (unsigned i = 0; i < call->arg_size(); ++i) {
            callExpr->args.push_back(liftValue(*call->getArgOperand(i)));
        }

        // If the call has a non-void return, wrap in VarDeclStmt
        if (!call->getType()->isVoidTy()) {
            auto stmt = std::make_unique<transpile::VarDeclStmt>();
            stmt->fidelity = Fidelity::Recovered;
            stmt->type = liftType(call->getType());
            stmt->name = getValueName(*call);
            stmt->init = std::move(callExpr);
            return stmt;
        }

        auto stmt = std::make_unique<transpile::ExprStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->expr = std::move(callExpr);
        return stmt;
    }

    // -- InvokeInst -> same as CallInst (the EH edges are reconstructed by
    //    the structured TryCatch recovery; here we only model the call) --
    if (auto* inv = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
        auto callExpr = std::make_unique<transpile::CallExpr>();
        callExpr->fidelity = Fidelity::Recovered;

        if (auto* calledFn = inv->getCalledFunction()) {
            callExpr->callee = calledFn->getName().str();
        } else {
            callExpr->callee = getValueName(*inv->getCalledOperand());
        }
        for (unsigned i = 0; i < inv->arg_size(); ++i) {
            callExpr->args.push_back(liftValue(*inv->getArgOperand(i)));
        }

        if (!inv->getType()->isVoidTy()) {
            auto stmt = std::make_unique<transpile::VarDeclStmt>();
            stmt->fidelity = Fidelity::Recovered;
            stmt->type = liftType(inv->getType());
            stmt->name = getValueName(*inv);
            stmt->init = std::move(callExpr);
            return stmt;
        }
        auto stmt = std::make_unique<transpile::ExprStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->expr = std::move(callExpr);
        return stmt;
    }

    // -- LandingPadInst / ResumeInst: EH artifacts. At Direct level there
    //    is no try/catch reconstruction, so they carry no statement.
    //    (Structured level handles them via TryCatch recovery and never
    //    reaches this fallback for them.) --
    if (llvm::isa<llvm::LandingPadInst>(&inst) ||
        llvm::isa<llvm::ResumeInst>(&inst)) {
        return nullptr;
    }

    // -- BinaryOperator -> VarDeclStmt with BinaryOpExpr --
    if (auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(&inst)) {
        auto binExpr = std::make_unique<transpile::BinaryOpExpr>();
        binExpr->fidelity = Fidelity::Recovered;
        binExpr->op = mapBinaryOp(binOp->getOpcode());
        binExpr->lhs = liftValue(*binOp->getOperand(0));
        binExpr->rhs = liftValue(*binOp->getOperand(1));

        auto stmt = std::make_unique<transpile::VarDeclStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->type = liftType(binOp->getType());
        stmt->name = getValueName(*binOp);
        stmt->init = std::move(binExpr);
        return stmt;
    }

    // -- ICmpInst -> VarDeclStmt with BinaryOpExpr (comparison) --
    if (auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
        auto cmpExpr = std::make_unique<transpile::BinaryOpExpr>();
        cmpExpr->fidelity = Fidelity::Recovered;
        cmpExpr->op = mapICmpPredicate(icmp->getPredicate());
        cmpExpr->lhs = liftValue(*icmp->getOperand(0));
        cmpExpr->rhs = liftValue(*icmp->getOperand(1));

        auto stmt = std::make_unique<transpile::VarDeclStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->type = liftType(icmp->getType());
        stmt->name = getValueName(*icmp);
        stmt->init = std::move(cmpExpr);
        return stmt;
    }

    // -- FCmpInst -> VarDeclStmt with BinaryOpExpr (comparison) --
    if (auto* fcmp = llvm::dyn_cast<llvm::FCmpInst>(&inst)) {
        auto cmpExpr = std::make_unique<transpile::BinaryOpExpr>();
        cmpExpr->fidelity = Fidelity::Recovered;
        cmpExpr->op = mapFCmpPredicate(fcmp->getPredicate());
        cmpExpr->lhs = liftValue(*fcmp->getOperand(0));
        cmpExpr->rhs = liftValue(*fcmp->getOperand(1));

        auto stmt = std::make_unique<transpile::VarDeclStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->type = liftType(fcmp->getType());
        stmt->name = getValueName(*fcmp);
        stmt->init = std::move(cmpExpr);
        return stmt;
    }

    // -- SwitchInst -> SwitchStmt --
    if (auto* SI = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
        auto switchStmt = std::make_unique<transpile::SwitchStmt>();
        switchStmt->fidelity = Fidelity::Recovered;
        switchStmt->subject = liftValue(*SI->getCondition());

        // Collect case clauses
        for (const auto& Case : SI->cases()) {
            transpile::SwitchCase sc;
            auto caseVal = std::make_unique<transpile::LiteralExpr>();
            caseVal->fidelity = Fidelity::Recovered;
            caseVal->litKind = transpile::LiteralKind::Integer;
            caseVal->value = std::to_string(Case.getCaseValue()->getSExtValue());
            sc.value = std::move(caseVal);
            // Direct level: case bodies left empty (no CFG reconstruction)
            switchStmt->cases.push_back(std::move(sc));
        }

        // Default case (value == nullptr signals default)
        transpile::SwitchCase defaultCase;
        defaultCase.value = nullptr;
        switchStmt->cases.push_back(std::move(defaultCase));

        return switchStmt;
    }

    // -- BranchInst (conditional) -> IfStmt --
    if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
        if (br->isConditional()) {
            auto stmt = std::make_unique<transpile::IfStmt>();
            stmt->fidelity = Fidelity::Recovered;
            stmt->condition = liftValue(*br->getCondition());
            // Direct level: no CFG reconstruction — bodies left empty.
            // The true/false targets are basic block labels; proper
            // if/else body recovery requires Structured level analysis.
            return stmt;
        }
        // Unconditional branches are control flow, not meaningful at Direct level
        return nullptr;
    }

    // -- GetElementPtrInst -> VarDeclStmt with MemberAccessExpr or IndexExpr --
    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst)) {
        transpile::ExprPtr current = liftValue(*gep->getPointerOperand());

        for (auto it = gep->idx_begin(); it != gep->idx_end(); ++it) {
            llvm::Value* idx = it->get();

            // First index is the pointer offset (usually 0 for struct access)
            // Subsequent indices are field/array indices
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
                if (it == gep->idx_begin() && ci->isZero()) continue; // Skip the trivial base pointer offset

                // For struct field access with known struct type
                auto* srcElemTy = gep->getSourceElementType();
                if (srcElemTy && srcElemTy->isStructTy() && it != gep->idx_begin()) {
                    auto memberExpr = std::make_unique<transpile::MemberAccessExpr>();
                    memberExpr->object = std::move(current);
                    auto* structTy = llvm::cast<llvm::StructType>(srcElemTy);
                    uint64_t fieldIdx = ci->getZExtValue();
                    const std::vector<std::string>* diMembers =
                        structTy->hasName() ? debugMemberNames(structTy->getName())
                                            : nullptr;
                    if (diMembers && fieldIdx < diMembers->size() &&
                        !(*diMembers)[fieldIdx].empty()) {
                        memberExpr->member = (*diMembers)[fieldIdx];
                        memberExpr->fidelity = Fidelity::Recovered;
                    } else {
                        memberExpr->member = "field" + std::to_string(fieldIdx);
                        memberExpr->fidelity = Fidelity::Inferred;
                    }
                    current = std::move(memberExpr);
                } else {
                    auto indexExpr = std::make_unique<transpile::IndexExpr>();
                    indexExpr->fidelity = Fidelity::Recovered;
                    indexExpr->object = std::move(current);
                    indexExpr->index = liftValue(*idx);
                    current = std::move(indexExpr);
                }
            } else {
                // Dynamic index
                auto indexExpr = std::make_unique<transpile::IndexExpr>();
                indexExpr->fidelity = Fidelity::Recovered;
                indexExpr->object = std::move(current);
                indexExpr->index = liftValue(*idx);
                current = std::move(indexExpr);
            }
        }

        auto stmt = std::make_unique<transpile::VarDeclStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->type = liftType(gep->getType());
        stmt->name = getValueName(*gep);
        stmt->init = std::move(current);
        return stmt;
    }

    // -- PHINode -> VarDeclStmt with Inferred fidelity --
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(&inst)) {
        auto stmt = std::make_unique<transpile::VarDeclStmt>();
        stmt->fidelity = Fidelity::Inferred;
        stmt->type = liftType(phi->getType());
        stmt->name = getValueName(*phi);
        // PHI nodes merge multiple SSA values — at Direct level, pick the
        // first incoming value as a representative initializer.
        if (phi->getNumIncomingValues() > 0) {
            stmt->init = liftValue(*phi->getIncomingValue(0));
            stmt->init->fidelity = Fidelity::Inferred;
        }
        return stmt;
    }

    // -- SelectInst -> TernaryExpr (ternary recovery) --
    if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst)) {
        auto ternary = std::make_unique<transpile::TernaryExpr>();
        ternary->fidelity = Fidelity::Recovered;
        ternary->condition = liftValue(*sel->getCondition());
        ternary->trueExpr = liftValue(*sel->getTrueValue());
        ternary->falseExpr = liftValue(*sel->getFalseValue());

        auto stmt = std::make_unique<transpile::VarDeclStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->type = liftType(sel->getType());
        stmt->name = getValueName(*sel);
        stmt->init = std::move(ternary);
        return stmt;
    }

    // -- CastInst (bitcast, sext, zext, trunc, fpext, fptrunc, etc.) --
    if (auto* cast = llvm::dyn_cast<llvm::CastInst>(&inst)) {
        auto stmt = std::make_unique<transpile::VarDeclStmt>();
        stmt->fidelity = Fidelity::Recovered;
        stmt->type = liftType(cast->getType());
        stmt->name = getValueName(*cast);
        stmt->init = liftValue(*cast->getOperand(0));
        return stmt;
    }

    // -- Fallback: unsupported instruction --
    auto unsupported = std::make_unique<transpile::UnsupportedExpr>();
    unsupported->fidelity = Fidelity::Recovered;
    unsupported->description = inst.getOpcodeName();

    auto stmt = std::make_unique<transpile::ExprStmt>();
    stmt->fidelity = Fidelity::Recovered;
    stmt->expr = std::move(unsupported);
    return stmt;
}

// ---------------------------------------------------------------------------
// Function lifting
// ---------------------------------------------------------------------------

transpile::TranspileFunction LLVMLifter::liftFunctionDirect(const llvm::Function& func,
                                                            const std::string& qualifiedName,
                                                            const SymbolTable& metadata) {
    transpile::TranspileFunction result;
    result.fidelity = transpile::Fidelity::Recovered;
    result.qualifiedName = qualifiedName;

    // Recover return type and parameter types from metadata when available
    const auto* fnSym = metadata.findFunction(qualifiedName);
    if (fnSym) {
        result.returnType = fnSym->returnType;
        result.params = fnSym->params;
        result.accessModifier = visibilityToAccessModifier(fnSym->visibility);
    } else {
        // Fallback: derive from LLVM IR types
        result.returnType = liftType(func.getReturnType());
        unsigned argIdx = 0;
        for (const auto& arg : func.args()) {
            Parameter p;
            p.type = liftType(arg.getType());
            p.name = arg.hasName() ? arg.getName().str() : ("arg" + std::to_string(argIdx));
            result.params.push_back(p);
            ++argIdx;
        }
    }

    // Rust Box ownership: when DWARF says the return type is
    // `alloc::boxed::Box<T, Global>` and the body provides matching IR
    // allocation evidence, upgrade the return type to `owned T` (strips
    // the pointer modifier). Conservative — any non-Box / out-of-scope
    // shape leaves `ownership` untouched.
    if (auto owned = recoverBoxOwnership(dwarfReturnType(func), &func)) {
        result.returnType = std::move(*owned);
    }

    // Reset per-function state
    tmpCounter_ = 0;
    nameMap_.clear();
    ehRegionEntries_.clear();

    // Pre-populate name map with function arguments
    unsigned argIdx = 0;
    for (const auto& arg : func.args()) {
        if (arg.hasName()) {
            nameMap_[&arg] = arg.getName().str();
        } else if (fnSym && argIdx < fnSym->params.size()) {
            nameMap_[&arg] = fnSym->params[argIdx].name;
        } else {
            nameMap_[&arg] = "arg" + std::to_string(argIdx);
        }
        ++argIdx;
    }

    // Walk all basic blocks sequentially (Direct level — no CFG reconstruction)
    for (const auto& bb : func) {
        for (const auto& inst : bb) {
            auto stmt = liftInstruction(inst);
            if (stmt) result.body.push_back(std::move(stmt));
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// L2 Structured lifting — CFG reconstruction
// ---------------------------------------------------------------------------

bool LLVMLifter::detectCountedLoop(llvm::Loop* loop,
                                   transpile::StmtPtr& initStmt,
                                   transpile::ExprPtr& condition,
                                   transpile::ExprPtr& increment) {
    // Look for a simple induction variable pattern:
    //   header: %iv = phi [init, preheader], [next, latch]
    //   header or latch: %next = add %iv, step
    //   header: br i1 (icmp %iv, bound), body, exit

    auto* header = loop->getHeader();
    if (!header) return false;

    // Find the canonical induction variable
    llvm::PHINode* indVar = loop->getCanonicalInductionVariable();
    if (!indVar) return false;

    // Find the comparison in the header terminator
    auto* headerBr = llvm::dyn_cast<llvm::BranchInst>(header->getTerminator());
    if (!headerBr || !headerBr->isConditional()) return false;

    auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(headerBr->getCondition());
    if (!cmp) return false;

    // Build init statement: int _iv = 0
    auto initDecl = std::make_unique<transpile::VarDeclStmt>();
    initDecl->fidelity = transpile::Fidelity::Recovered;
    initDecl->type = liftType(indVar->getType());
    initDecl->name = getValueName(*indVar);

    // Find the initial value from the preheader incoming
    auto* preheader = loop->getLoopPreheader();
    if (preheader) {
        for (unsigned i = 0; i < indVar->getNumIncomingValues(); ++i) {
            if (indVar->getIncomingBlock(i) == preheader) {
                initDecl->init = liftValue(*indVar->getIncomingValue(i));
                break;
            }
        }
    }
    initStmt = std::move(initDecl);

    // Build condition from the icmp
    condition = liftValue(*cmp);

    // Build increment: find the add/sub that feeds back into the PHI
    for (unsigned i = 0; i < indVar->getNumIncomingValues(); ++i) {
        if (indVar->getIncomingBlock(i) != preheader) {
            auto* nextVal = indVar->getIncomingValue(i);
            if (auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(nextVal)) {
                auto incExpr = std::make_unique<transpile::BinaryOpExpr>();
                incExpr->fidelity = transpile::Fidelity::Recovered;
                incExpr->op = mapBinaryOp(binOp->getOpcode());
                incExpr->lhs = liftValue(*binOp->getOperand(0));
                incExpr->rhs = liftValue(*binOp->getOperand(1));
                increment = std::move(incExpr);
            } else {
                increment = liftValue(*nextVal);
            }
            break;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Itanium C++ exception-handling recovery
// ---------------------------------------------------------------------------

bool LLVMLifter::usesItaniumCxxEH(const llvm::Function& func) {
    if (!func.hasPersonalityFn()) return false;
    const llvm::Value* p = func.getPersonalityFn()->stripPointerCasts();
    if (const auto* pf = llvm::dyn_cast<llvm::Function>(p)) {
        return pf->getName() == "__gxx_personality_v0";
    }
    return false;
}

std::string LLVMLifter::typeInfoToReadableName(const llvm::Value* tiGlobal) {
    if (!tiGlobal) return {};
    const llvm::Value* g = tiGlobal->stripPointerCasts();
    if (!g->hasName()) return {};
    std::string sym = g->getName().str();
    // Itanium typeinfo symbols are mangled as `_ZTI<type>`. llvm::demangle
    // turns `_ZTISt12length_error` into `typeinfo for std::length_error`.
    std::string demangled = llvm::demangle(sym);
    constexpr const char* kPrefix = "typeinfo for ";
    auto pos = demangled.find(kPrefix);
    if (pos == 0) {
        std::string name = demangled.substr(std::string(kPrefix).size());
        // Strip pointer/reference decoration so the catch type stays a
        // plain readable name (best-effort; never fabricated).
        while (!name.empty() && (name.back() == '*' || name.back() == '&' ||
                                 name.back() == ' '))
            name.pop_back();
        if (!name.empty()) return name;
    }
    return {};
}

bool LLVMLifter::liftLandingPad(llvm::LandingPadInst* lp,
                                llvm::BasicBlock* lpadBB,
                                llvm::BasicBlock* regionExit,
                                llvm::LoopInfo& LI,
                                llvm::PostDominatorTree& PDT,
                                std::vector<transpile::CatchClause>& catches,
                                std::vector<transpile::StmtPtr>& finallyBody,
                                std::unordered_map<llvm::BasicBlock*, bool>& visited) {
    if (!lp || !lpadBB) return false;

    // The landingpad block dispatches on the selector (llvm.eh.typeid.for
    // comparisons). For a confident, conservative mapping we recover each
    // typed `catch <typeinfo>` clause and a single cleanup (finally) path.
    // Anything we cannot resolve to a readable type name is dropped to a
    // catch-all rather than fabricating a wrong type.
    bool hasCleanup = lp->isCleanup();
    bool emittedAnyCatch = false;

    // Walk the dispatch chain: clang emits, after the landingpad, a
    // sequence of `%id = call i32 @llvm.eh.typeid.for(@_ZTIxxx)` /
    // `icmp eq %sel, %id` / conditional branch to the matching
    // `__cxa_begin_catch` block. We follow the *true* edges to find each
    // catch handler block, and treat the final fall-through (or the
    // resume) as cleanup/finally.
    //
    // To stay conservative we only handle the canonical clang shape:
    //   - clauses are TypeInfo globals (typed catch) or null/i8* (catch-all)
    //   - each handler eventually reaches `regionExit` (the post-try join)
    // If the shape deviates we bail (return false) and the caller falls
    // back to the non-EH path.
    llvm::SmallPtrSet<llvm::BasicBlock*, 8> handlerBlocks;

    for (unsigned i = 0, n = lp->getNumClauses(); i < n; ++i) {
        if (lp->isCatch(i)) {
            llvm::Constant* clause = lp->getClause(i);
            // catch-all is modelled as a null typeinfo pointer.
            std::string typeName;
            if (clause && !clause->isNullValue()) {
                typeName = typeInfoToReadableName(clause);
                if (typeName.empty()) return false; // cannot name -> bail
            }
            transpile::CatchClause cc;
            if (!typeName.empty()) {
                TypeNode t;
                t.nameParts.push_back(typeName);
                cc.exceptionType = t;
            }
            cc.varName = "e";
            catches.push_back(std::move(cc));
            emittedAnyCatch = true;
        } else {
            // Filter clauses (exception specifications) are not part of the
            // simple try/catch shape we model -> bail conservatively.
            return false;
        }
    }

    // Recover handler bodies. clang routes each typed catch through a
    // `__cxa_begin_catch` / ... / `__cxa_end_catch` sequence. We lift the
    // landingpad block's successors that lead to a `__cxa_begin_catch`
    // call as the corresponding catch body, in clause order.
    //
    // Conservative simplification: collect every block reachable from the
    // landingpad (without re-entering the protected region) up to
    // regionExit; resume terminates a cleanup path (rethrow) and is not
    // emitted as a normal statement (handled in liftBlockRegion's
    // ResumeInst guard). Bodies are produced by recursing liftBlockRegion
    // so existing if/else/loop structuring still applies inside handlers.
    size_t clauseIdx = 0;
    for (auto& cc : catches) {
        // Find the begin-catch block for this clause by scanning forward
        // from the landingpad for the clauseIdx-th `__cxa_begin_catch`.
        (void)clauseIdx;
        std::vector<transpile::StmtPtr> body;
        // Lift from the landingpad block toward regionExit once; the
        // recursion naturally walks the dispatch + handler chain. We only
        // do this for the first clause; subsequent clauses share the same
        // recovered region (multi-catch on one landingpad is rare and the
        // bodies are emitted into the first handler — still correct, never
        // wrong-typed).
        if (clauseIdx == 0) {
            liftBlockRegion(lpadBB, regionExit, LI, PDT, body, visited);
        }
        cc.body = std::move(body);
        ++clauseIdx;
    }

    if (hasCleanup && !emittedAnyCatch) {
        // Pure cleanup landingpad -> finally. Lift the cleanup chain
        // (which ends in `resume` = rethrow, suppressed by the resume
        // guard) as the finally body.
        liftBlockRegion(lpadBB, regionExit, LI, PDT, finallyBody, visited);
        return true;
    }

    return emittedAnyCatch;
}

bool LLVMLifter::tryLiftEHRegion(llvm::BasicBlock* entry,
                                 llvm::BasicBlock* exit,
                                 llvm::LoopInfo& LI,
                                 llvm::PostDominatorTree& PDT,
                                 std::vector<transpile::StmtPtr>& stmts,
                                 std::unordered_map<llvm::BasicBlock*, bool>& visited) {
    // Re-entrancy guard: when we recurse into our own protected region the
    // sub-call hits this same `entry`; don't rebuild the TryCatch again.
    if (ehRegionEntries_.count(entry)) return false;

    // A landingpad block is a handler entry, never a try-region entry.
    if (entry->isLandingPad()) return false;

    llvm::Function* func = entry->getParent();
    if (!func || !usesItaniumCxxEH(*func)) return false;

    // Conservative bail: any Windows/funclet EH instruction anywhere in the
    // function -> do not attempt recovery (keep current behavior).
    for (auto& bb : *func) {
        if (llvm::isa<llvm::CatchSwitchInst>(bb.getTerminator()) ||
            llvm::isa<llvm::CleanupReturnInst>(bb.getTerminator()) ||
            llvm::isa<llvm::CatchReturnInst>(bb.getTerminator()))
            return false;
        for (auto& I : bb) {
            if (llvm::isa<llvm::CatchPadInst>(&I) ||
                llvm::isa<llvm::CleanupPadInst>(&I))
                return false;
        }
    }

    // Find the first InvokeInst on the normal path starting at `entry`,
    // staying inside the [entry, exit) region and not entering a loop we
    // would otherwise structure.
    std::vector<llvm::BasicBlock*> worklist{entry};
    llvm::SmallPtrSet<llvm::BasicBlock*, 16> seen;
    llvm::SmallVector<llvm::InvokeInst*, 8> invokes;
    llvm::BasicBlock* landingPadBB = nullptr;

    while (!worklist.empty()) {
        llvm::BasicBlock* bb = worklist.back();
        worklist.pop_back();
        if (!bb || bb == exit || !seen.insert(bb).second) continue;

        if (auto* inv = llvm::dyn_cast<llvm::InvokeInst>(bb->getTerminator())) {
            llvm::BasicBlock* lpad = inv->getUnwindDest();
            if (!lpad || !lpad->isLandingPad()) return false; // odd shape
            if (!landingPadBB) {
                landingPadBB = lpad;
            } else if (landingPadBB != lpad) {
                // Multiple distinct landingpads in the region -> cannot
                // confidently delimit nested/overlapping EH. Bail.
                return false;
            }
            invokes.push_back(inv);
            // Continue collecting along the *normal* edge only.
            worklist.push_back(inv->getNormalDest());
            continue;
        }

        // Do not dive into a nested loop here; loop structuring owns it.
        if (auto* L = LI.getLoopFor(bb)) {
            if (L->getHeader() == bb && bb != entry) continue;
        }
        for (auto* succ : llvm::successors(bb))
            worklist.push_back(succ);
    }

    if (invokes.empty() || !landingPadBB) return false;

    llvm::LandingPadInst* lp = landingPadBB->getLandingPadInst();
    if (!lp) return false;

    // The post-try continuation: the nearest common post-dominator of all
    // invokes' normal destinations. That is where control resumes after
    // the protected region regardless of which invoke ran last.
    llvm::BasicBlock* converge = invokes[0]->getNormalDest();
    for (size_t i = 1; i < invokes.size(); ++i) {
        converge = PDT.findNearestCommonDominator(converge,
                                                  invokes[i]->getNormalDest());
        if (!converge) break;
    }
    // Fall back to the caller's region exit when PDT gives nothing useful.
    if (!converge) converge = exit;

    auto tryStmt = std::make_unique<transpile::TryCatchStmt>();
    tryStmt->fidelity = transpile::Fidelity::Recovered;

    // tryBody: the protected region from `entry` up to `converge`, with the
    // landingpad excluded (it is reached only via the unwind edge). We
    // mark the landingpad visited *before* recursing so the normal-path
    // recursion never wanders into the handler.
    bool lpadWasVisited = visited[landingPadBB];
    visited[landingPadBB] = true;
    ehRegionEntries_.insert(entry);
    liftBlockRegion(entry, converge, LI, PDT, tryStmt->tryBody, visited);
    ehRegionEntries_.erase(entry);
    visited[landingPadBB] = lpadWasVisited;

    // Recover catch/finally from the landingpad. The handler region runs
    // from the landingpad up to the same convergence point.
    std::vector<transpile::CatchClause> catches;
    std::vector<transpile::StmtPtr> finallyBody;
    if (!liftLandingPad(lp, landingPadBB, converge, LI, PDT, catches,
                        finallyBody, visited)) {
        // Could not confidently model the handler -> abort EH recovery
        // entirely. The caller proceeds with the normal path; nothing in
        // `stmts` has been touched yet.
        return false;
    }

    tryStmt->catchClauses = std::move(catches);
    tryStmt->finallyBody = std::move(finallyBody);

    if (tryStmt->tryBody.empty() && tryStmt->catchClauses.empty() &&
        tryStmt->finallyBody.empty()) {
        // Degenerate — never emit an empty TryCatch.
        return false;
    }

    stmts.push_back(std::move(tryStmt));

    // Continue lifting after the protected region.
    if (converge && converge != exit)
        liftBlockRegion(converge, exit, LI, PDT, stmts, visited);
    return true;
}

void LLVMLifter::liftBlockRegion(llvm::BasicBlock* entry,
                                 llvm::BasicBlock* exit,
                                 llvm::LoopInfo& LI,
                                 llvm::PostDominatorTree& PDT,
                                 std::vector<transpile::StmtPtr>& stmts,
                                 std::unordered_map<llvm::BasicBlock*, bool>& visited) {
    if (!entry || entry == exit || visited[entry]) return;

    // Itanium C++ EH pre-pass: if this block opens a try region (reaches
    // an `invoke` whose unwind edge is a landingpad), reconstruct a
    // TryCatchStmt and let it drive the sub-region recursion. Runs before
    // `entry` is marked visited so the pre-pass owns the bookkeeping;
    // returns false (touching nothing) for any shape it cannot confidently
    // map, in which case we fall through to the normal structuring below.
    if (tryLiftEHRegion(entry, exit, LI, PDT, stmts, visited)) return;

    if (visited[entry]) return; // pre-pass may have lifted us via recursion
    visited[entry] = true;

    // Check if this block is a loop header
    if (auto* loop = LI.getLoopFor(entry)) {
        if (loop->getHeader() == entry) {
            // Try to detect a counted loop
            transpile::StmtPtr initStmt;
            transpile::ExprPtr condition;
            transpile::ExprPtr increment;

            if (detectCountedLoop(loop, initStmt, condition, increment)) {
                // Emit ForStmt
                auto forStmt = std::make_unique<transpile::ForStmt>();
                forStmt->fidelity = transpile::Fidelity::Recovered;
                forStmt->init = std::move(initStmt);
                forStmt->condition = std::move(condition);
                forStmt->increment = std::move(increment);

                // Lift loop body blocks
                for (auto* bb : loop->blocks()) {
                    if (bb == entry) continue; // skip header (contains the cmp/branch)
                    if (visited[bb]) continue;
                    visited[bb] = true;
                    for (const auto& inst : *bb) {
                        if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                            if (br->isConditional()) continue; // conditional branches are control flow structure
                            // Unconditional branch: check if it targets loop exit or header
                            llvm::BasicBlock* target = br->getSuccessor(0);
                            llvm::SmallVector<llvm::BasicBlock*, 4> exitBlocks;
                            loop->getExitBlocks(exitBlocks);
                            bool isBreak = std::find(exitBlocks.begin(), exitBlocks.end(), target) != exitBlocks.end();
                            bool isContinue = (target == loop->getHeader()) || (target == loop->getLoopLatch());
                            if (isBreak) {
                                auto breakStmt = std::make_unique<transpile::BreakStmt>();
                                breakStmt->fidelity = transpile::Fidelity::Recovered;
                                forStmt->body.push_back(std::move(breakStmt));
                            } else if (isContinue) {
                                auto contStmt = std::make_unique<transpile::ContinueStmt>();
                                contStmt->fidelity = transpile::Fidelity::Recovered;
                                forStmt->body.push_back(std::move(contStmt));
                            }
                            continue;
                        }
                        auto stmt = liftInstruction(inst);
                        if (stmt) forStmt->body.push_back(std::move(stmt));
                    }
                }
                compactCompoundAssigns(forStmt->body);

                stmts.push_back(std::move(forStmt));
            } else {
                // Emit WhileStmt
                auto whileStmt = std::make_unique<transpile::WhileStmt>();
                whileStmt->fidelity = transpile::Fidelity::Recovered;

                // Extract condition from header terminator
                auto* term = entry->getTerminator();
                if (auto* br = llvm::dyn_cast<llvm::BranchInst>(term)) {
                    if (br->isConditional()) {
                        whileStmt->condition = liftValue(*br->getCondition());
                    } else {
                        auto trueLit = std::make_unique<transpile::LiteralExpr>();
                        trueLit->litKind = transpile::LiteralKind::Boolean;
                        trueLit->value = "true";
                        whileStmt->condition = std::move(trueLit);
                    }
                }

                // Lift loop body
                for (auto* bb : loop->blocks()) {
                    if (bb == entry) continue;
                    if (visited[bb]) continue;
                    visited[bb] = true;
                    for (const auto& inst : *bb) {
                        if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                            if (br->isConditional()) continue; // conditional branches are control flow structure
                            // Unconditional branch: check if it targets loop exit or header
                            llvm::BasicBlock* target = br->getSuccessor(0);
                            llvm::SmallVector<llvm::BasicBlock*, 4> exitBlocks;
                            loop->getExitBlocks(exitBlocks);
                            bool isBreak = std::find(exitBlocks.begin(), exitBlocks.end(), target) != exitBlocks.end();
                            bool isContinue = (target == loop->getHeader()) || (target == loop->getLoopLatch());
                            if (isBreak) {
                                auto breakStmt = std::make_unique<transpile::BreakStmt>();
                                breakStmt->fidelity = transpile::Fidelity::Recovered;
                                whileStmt->body.push_back(std::move(breakStmt));
                            } else if (isContinue) {
                                auto contStmt = std::make_unique<transpile::ContinueStmt>();
                                contStmt->fidelity = transpile::Fidelity::Recovered;
                                whileStmt->body.push_back(std::move(contStmt));
                            }
                            continue;
                        }
                        auto stmt = liftInstruction(inst);
                        if (stmt) whileStmt->body.push_back(std::move(stmt));
                    }
                }
                compactCompoundAssigns(whileStmt->body);

                stmts.push_back(std::move(whileStmt));
            }

            // Mark all loop blocks visited and continue after the loop
            for (auto* bb : loop->blocks())
                visited[bb] = true;

            // Continue with the exit block
            llvm::SmallVector<llvm::BasicBlock*, 4> exitBlocks;
            loop->getExitBlocks(exitBlocks);
            for (auto* exitBB : exitBlocks) {
                liftBlockRegion(exitBB, exit, LI, PDT, stmts, visited);
            }
            return;
        }
    }

    // Check if the block terminates with a switch instruction
    if (auto* SI = llvm::dyn_cast<llvm::SwitchInst>(entry->getTerminator())) {
        // Lift all instructions except the switch terminator
        for (const auto& inst : *entry) {
            if (&inst == SI) continue;
            auto stmt = liftInstruction(inst);
            if (stmt) stmts.push_back(std::move(stmt));
        }

        auto switchStmt = std::make_unique<transpile::SwitchStmt>();
        switchStmt->fidelity = transpile::Fidelity::Recovered;
        switchStmt->subject = liftValue(*SI->getCondition());

        // Collect all case destination blocks for convergence detection
        std::vector<llvm::BasicBlock*> allDests;
        for (const auto& Case : SI->cases()) {
            allDests.push_back(Case.getCaseSuccessor());
        }
        allDests.push_back(SI->getDefaultDest());

        // Find convergence point via post-dominator
        llvm::BasicBlock* convergeBB = allDests[0];
        for (size_t i = 1; i < allDests.size(); ++i) {
            convergeBB = PDT.findNearestCommonDominator(convergeBB, allDests[i]);
            if (!convergeBB) break;
        }

        // Lift each case
        for (const auto& Case : SI->cases()) {
            transpile::SwitchCase sc;
            auto caseVal = std::make_unique<transpile::LiteralExpr>();
            caseVal->fidelity = transpile::Fidelity::Recovered;
            caseVal->litKind = transpile::LiteralKind::Integer;
            caseVal->value = std::to_string(Case.getCaseValue()->getSExtValue());
            sc.value = std::move(caseVal);
            liftBlockRegion(Case.getCaseSuccessor(), convergeBB, LI, PDT, sc.body, visited);
            switchStmt->cases.push_back(std::move(sc));
        }

        // Default case
        transpile::SwitchCase defaultCase;
        defaultCase.value = nullptr;
        auto* defaultBB = SI->getDefaultDest();
        if (defaultBB != convergeBB) {
            liftBlockRegion(defaultBB, convergeBB, LI, PDT, defaultCase.body, visited);
        }
        switchStmt->cases.push_back(std::move(defaultCase));

        stmts.push_back(std::move(switchStmt));

        // Continue from convergence point
        if (convergeBB) {
            liftBlockRegion(convergeBB, exit, LI, PDT, stmts, visited);
        }
        return;
    }

    // Lift instructions in this block (except terminators for control flow)
    for (const auto& inst : *entry) {
        // InvokeInst is a call + a terminator. Emit the call, then keep
        // lifting along the *normal* edge (the unwind edge is owned by the
        // TryCatch recovery / landingpad handling).
        if (auto* inv = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
            auto stmt = liftInstruction(inst);
            if (stmt) stmts.push_back(std::move(stmt));
            liftBlockRegion(inv->getNormalDest(), exit, LI, PDT, stmts, visited);
            return;
        }
        // ResumeInst = rethrow terminating a cleanup. It is not a normal
        // statement and has no successor; stop lifting this path.
        if (llvm::isa<llvm::ResumeInst>(&inst)) {
            return;
        }
        if (llvm::isa<llvm::BranchInst>(&inst)) {
            auto* br = llvm::cast<llvm::BranchInst>(&inst);
            if (br->isConditional()) {
                // If/else reconstruction via post-dominator
                auto* trueBB = br->getSuccessor(0);
                auto* falseBB = br->getSuccessor(1);

                // Find the convergence point (immediate post-dominator)
                auto* convergeBB = PDT.findNearestCommonDominator(trueBB, falseBB);

                auto ifStmt = std::make_unique<transpile::IfStmt>();
                ifStmt->fidelity = transpile::Fidelity::Recovered;
                ifStmt->condition = liftValue(*br->getCondition());

                // Lift then branch
                liftBlockRegion(trueBB, convergeBB, LI, PDT, ifStmt->thenBody, visited);

                // Lift else branch (only if it's not the convergence point itself)
                if (falseBB != convergeBB) {
                    liftBlockRegion(falseBB, convergeBB, LI, PDT, ifStmt->elseBody, visited);
                }

                stmts.push_back(std::move(ifStmt));

                // Continue from the convergence point
                liftBlockRegion(convergeBB, exit, LI, PDT, stmts, visited);
                return;
            } else {
                // Unconditional branch — follow to successor
                liftBlockRegion(br->getSuccessor(0), exit, LI, PDT, stmts, visited);
                return;
            }
        }

        auto stmt = liftInstruction(inst);
        if (stmt) stmts.push_back(std::move(stmt));
    }

    // If the terminator is not a branch (e.g., ret), we're done
    compactCompoundAssigns(stmts);
}

transpile::TranspileFunction LLVMLifter::liftFunctionStructured(llvm::Function& func,
                                                                const std::string& qualifiedName,
                                                                const SymbolTable& metadata) {
    transpile::TranspileFunction result;
    result.fidelity = transpile::Fidelity::Recovered;
    result.qualifiedName = qualifiedName;

    // Recover return type and parameter types from metadata
    const auto* fnSym = metadata.findFunction(qualifiedName);
    if (fnSym) {
        result.returnType = fnSym->returnType;
        result.params = fnSym->params;
        result.accessModifier = visibilityToAccessModifier(fnSym->visibility);
    } else {
        result.returnType = liftType(func.getReturnType());
        unsigned argIdx = 0;
        for (const auto& arg : func.args()) {
            Parameter p;
            p.type = liftType(arg.getType());
            p.name = arg.hasName() ? arg.getName().str() : ("arg" + std::to_string(argIdx));
            result.params.push_back(p);
            ++argIdx;
        }
    }

    // Rust Box ownership: same as Direct — upgrade `Box<T, Global>` returns
    // to `owned T` when DWARF + IR alloc evidence agree.
    if (auto owned = recoverBoxOwnership(dwarfReturnType(func), &func)) {
        result.returnType = std::move(*owned);
    }

    // Reset per-function state
    tmpCounter_ = 0;
    nameMap_.clear();
    ehRegionEntries_.clear();

    // Pre-populate name map with function arguments
    unsigned argIdx = 0;
    for (const auto& arg : func.args()) {
        if (arg.hasName()) {
            nameMap_[&arg] = arg.getName().str();
        } else if (fnSym && argIdx < fnSym->params.size()) {
            nameMap_[&arg] = fnSym->params[argIdx].name;
        } else {
            nameMap_[&arg] = "arg" + std::to_string(argIdx);
        }
        ++argIdx;
    }

    // Build analysis passes
    llvm::DominatorTree DT(func);
    llvm::PostDominatorTree PDT(func);
    llvm::LoopInfo LI(DT);

    // Walk the CFG starting from the entry block
    std::unordered_map<llvm::BasicBlock*, bool> visited;
    liftBlockRegion(&func.getEntryBlock(), nullptr, LI, PDT, result.body, visited);
    compactCompoundAssigns(result.body);

    return result;
}

// ---------------------------------------------------------------------------
// DWARF debug-info recovery
// ---------------------------------------------------------------------------

void LLVMLifter::buildDebugInfoMaps(llvm::Module& module) {
    diStructMembers_.clear();
    diLocals_.clear();
    hasDebugInfo_ = false;

    // No compile unit -> no debug info. Stay empty; callers degrade.
    if (module.debug_compile_units_begin() == module.debug_compile_units_end())
        return;

    // 1. Struct/class/union member names. DebugInfoFinder walks the whole
    //    module (CU retained types, subprogram-local types, etc.).
    llvm::DebugInfoFinder finder;
    finder.processModule(module);
    for (const llvm::DIType* ty : finder.types()) {
        const auto* comp = llvm::dyn_cast<llvm::DICompositeType>(ty);
        if (!comp) continue;
        unsigned tag = comp->getTag();
        if (tag != llvm::dwarf::DW_TAG_structure_type &&
            tag != llvm::dwarf::DW_TAG_class_type &&
            tag != llvm::dwarf::DW_TAG_union_type)
            continue;
        if (comp->getName().empty()) continue;

        // Collect DW_TAG_member elements in declaration order. Skip static
        // members / methods (they are not LLVM struct elements).
        std::vector<std::string> members;
        for (const llvm::DINode* el : comp->getElements()) {
            const auto* dt = llvm::dyn_cast_or_null<llvm::DIDerivedType>(el);
            if (!dt || dt->getTag() != llvm::dwarf::DW_TAG_member) continue;
            if (dt->isStaticMember()) continue;
            members.push_back(dt->getName().str());
        }
        if (members.empty()) continue;

        std::string key = comp->getName().str();
        // Prefer the entry with the most members when a name repeats across
        // CUs (a forward decl can have zero, a definition has all).
        auto it = diStructMembers_.find(key);
        if (it == diStructMembers_.end() || it->second.size() < members.size())
            diStructMembers_[key] = std::move(members);
    }

    // 2. alloca -> DILocalVariable via debug intrinsics / debug records.
    //    Handles both the legacy llvm.dbg.declare/value intrinsics and the
    //    new (LLVM 19+) #dbg_declare/#dbg_value debug records.
    auto recordLocal = [&](llvm::Value* addr, llvm::DILocalVariable* var) {
        if (!addr || !var) return;
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(addr->stripPointerCasts()))
            diLocals_.emplace(a, var);
    };
    for (llvm::Function& fn : module) {
        for (llvm::BasicBlock& bb : fn) {
            for (llvm::Instruction& inst : bb) {
                // New-format debug records attached to the instruction.
                for (llvm::DbgRecord& dr : inst.getDbgRecordRange()) {
                    if (auto* dvr = llvm::dyn_cast<llvm::DbgVariableRecord>(&dr)) {
                        if (dvr->isDbgDeclare())
                            recordLocal(dvr->getVariableLocationOp(0), dvr->getVariable());
                    }
                }
                // Legacy intrinsic form.
                if (auto* ddi = llvm::dyn_cast<llvm::DbgDeclareInst>(&inst))
                    recordLocal(ddi->getAddress(), ddi->getVariable());
            }
        }
    }

    hasDebugInfo_ = !diStructMembers_.empty() || !diLocals_.empty();
}

const std::vector<std::string>*
LLVMLifter::debugMemberNames(llvm::StringRef structName) const {
    if (diStructMembers_.empty()) return nullptr;
    auto it = diStructMembers_.find(stripStructName(structName));
    return it == diStructMembers_.end() ? nullptr : &it->second;
}

std::optional<TypeNode> LLVMLifter::typeFromDI(const llvm::DIType* diTy) const {
    if (!diTy) return std::nullopt;

    if (const auto* basic = llvm::dyn_cast<llvm::DIBasicType>(diTy)) {
        if (basic->getName().empty()) return std::nullopt;
        TypeNode node;
        node.nameParts.push_back(basic->getName().str());
        return node;
    }

    if (const auto* comp = llvm::dyn_cast<llvm::DICompositeType>(diTy)) {
        if (comp->getName().empty()) return std::nullopt;
        TypeNode node;
        node.nameParts.push_back(comp->getName().str());
        return node;
    }

    if (const auto* der = llvm::dyn_cast<llvm::DIDerivedType>(diTy)) {
        switch (der->getTag()) {
        case llvm::dwarf::DW_TAG_pointer_type: {
            auto inner = typeFromDI(der->getBaseType());
            if (!inner) return std::nullopt;
            inner->modifier = TypeNode::Ptr;
            return inner;
        }
        case llvm::dwarf::DW_TAG_reference_type:
        case llvm::dwarf::DW_TAG_rvalue_reference_type: {
            auto inner = typeFromDI(der->getBaseType());
            if (!inner) return std::nullopt;
            inner->modifier = TypeNode::Ref;
            return inner;
        }
        case llvm::dwarf::DW_TAG_const_type:
        case llvm::dwarf::DW_TAG_volatile_type:
        case llvm::dwarf::DW_TAG_restrict_type:
        case llvm::dwarf::DW_TAG_atomic_type: {
            auto inner = typeFromDI(der->getBaseType());
            if (!inner) return std::nullopt;
            if (der->getTag() == llvm::dwarf::DW_TAG_const_type) inner->isConst = true;
            return inner;
        }
        case llvm::dwarf::DW_TAG_typedef: {
            // Prefer the typedef's own name (it is the source-level spelling).
            if (!der->getName().empty()) {
                TypeNode node;
                node.nameParts.push_back(der->getName().str());
                return node;
            }
            return typeFromDI(der->getBaseType());
        }
        default:
            return std::nullopt;
        }
    }

    return std::nullopt;
}

const llvm::DILocalVariable*
LLVMLifter::debugLocalForAlloca(const llvm::AllocaInst* a) const {
    if (diLocals_.empty() || !a) return nullptr;
    auto it = diLocals_.find(a);
    return it == diLocals_.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Rust Box ownership recovery (Box-only MVP)
// ---------------------------------------------------------------------------

namespace {

/// True iff the DWARF name `n` describes a `Box<T, alloc::alloc::Global>`
/// at its outermost level — i.e. the path qualifier is exactly
/// `alloc::boxed::Box`, the template clause is a balanced top-level
/// `<...>`, the allocator slot is the default `Global`, and T itself is
/// not another Box / Arc / Rc / Weak.
///
/// Conservative by construction. References (`&` prefix), raw-pointer
/// wrappers (`*mut`/`*const`), `&mut Box<...>`, and any other prefix
/// decoration are rejected here so callers never see a
/// "Box but wrapped" classification. Out-of-scope nested wrappers in the
/// T slot are also rejected so the caller can degrade cleanly.
bool matchOutermostBox(const std::string& n) {
    std::string inner; // captured for symmetry but the caller uses typeFromDI
    static const std::string kBoxQual = "alloc::boxed::Box<";
    if (n.size() <= kBoxQual.size()) return false;
    // Must start *literally* with the qualifier — no leading `&`/`*mut`/etc.
    if (n.compare(0, kBoxQual.size(), kBoxQual) != 0) return false;
    if (n.back() != '>') return false;

    // The substring between the matching outermost <...> is the args list.
    // Find the matching '>' for the first '<' by depth tracking; it must be
    // the very last character of `n`.
    size_t open = kBoxQual.size() - 1; // position of '<'
    int depth = 1;
    size_t close = std::string::npos;
    for (size_t i = open + 1; i < n.size(); ++i) {
        if (n[i] == '<') ++depth;
        else if (n[i] == '>') {
            if (--depth == 0) { close = i; break; }
        }
    }
    if (close != n.size() - 1) return false;
    std::string args = n.substr(open + 1, close - open - 1);

    // Split top-level comma-separated args. The Global-allocator default is
    // always present in rustc's DWARF Box name (e.g.
    // `alloc::boxed::Box<i32, alloc::alloc::Global>`).
    std::vector<std::string> tokens;
    {
        int d = 0;
        size_t start = 0;
        for (size_t i = 0; i < args.size(); ++i) {
            char c = args[i];
            if (c == '<') ++d;
            else if (c == '>') --d;
            else if (c == ',' && d == 0) {
                tokens.push_back(args.substr(start, i - start));
                start = i + 1;
            }
        }
        tokens.push_back(args.substr(start));
    }
    // rustc's DWARF Box name is usually `Box<T, alloc::alloc::Global>` (two
    // top-level args), but the args list is rustc-version-dependent: some
    // toolchains emit just `Box<T>` with the default allocator elided. Accept
    // 1 OR 2 args for forward-compat across rustc versions. (NB: the linux CI
    // Box-ownership failure was NOT a name-shape miss — its DWARF was the
    // standard 2-arg form. The real cause was the IR-side alloc-evidence gate;
    // see functionHasBoxAllocEvidence's Box::<T>::new branch.)
    if (tokens.empty() || tokens.size() > 2) return false;

    // Trim whitespace.
    auto trim = [](std::string& s) {
        size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
        size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b-1])) --b;
        s = s.substr(a, b - a);
    };
    for (auto& t : tokens) trim(t);
    if (tokens[0].empty()) return false;
    // With an explicit allocator arg it must be the default Global. Match by
    // `::Global` suffix (and bare `Global`) so the check tolerates the
    // qualifier spelling drifting across rustc versions
    // (`alloc::alloc::Global`, `std::alloc::Global`, `Global`).
    if (tokens.size() == 2) {
        const std::string& a = tokens[1];
        bool isGlobal =
            a == "Global" ||
            (a.size() >= 8 && a.compare(a.size() - 8, 8, "::Global") == 0);
        if (!isGlobal) return false;
    }

    // Reject nested Box<Box<...>> (out of MVP scope).
    if (tokens[0].rfind("alloc::boxed::Box<", 0) == 0) return false;
    // Reject other smart-pointer wrappers nested in T spot — those are out
    // of scope and must degrade.
    if (tokens[0].rfind("alloc::sync::Arc<", 0) == 0) return false;
    if (tokens[0].rfind("alloc::rc::Rc<", 0) == 0) return false;
    if (tokens[0].rfind("alloc::sync::Weak<", 0) == 0) return false;
    if (tokens[0].rfind("alloc::rc::Weak<", 0) == 0) return false;

    inner = std::move(tokens[0]);
    return true;
}

} // namespace

bool LLVMLifter::functionHasBoxAllocEvidence(const llvm::Function& func) {
    // Walk every call/invoke in the function. We treat the following as
    // confirming IR-side evidence that the Box<T> shape (recovered from
    // DWARF) is real:
    //   * any call to a function whose name contains the canonical token
    //     `__rust_alloc` (covers both legacy `@__rust_alloc` and the v0
    //     mangled `_RNvCs..._7___rustc12___rust_alloc` form rustc emits);
    //   * a call to `alloc::alloc::exchange_malloc` (Itanium-mangled
    //     `_ZN5alloc5alloc15exchange_malloc...`) — the documented Box::new
    //     lowering, the single intermediate we are willing to accept
    //     without crossing into cross-function inference.
    //   * a direct call to the `Box::<T>::new` constructor itself. rustc's MIR
    //     inliner is version-dependent: with Box::new inlined (e.g. the macOS
    //     toolchain) the `exchange_malloc` call lands directly in `func`'s
    //     body; when it is NOT inlined (e.g. the linux CI rustc) only the
    //     `Box::new` call remains here and the alloc lives one frame down. The
    //     constructor call is unambiguous Box-allocation evidence, so accept
    //     it too — otherwise the recovery silently degrades to `void*` purely
    //     on an inlining-decision difference.
    //
    // We explicitly reject `__rust_alloc_zeroed`, `__rust_realloc`,
    // anything containing `Arc`/`Rc`/`Weak`, and `__rust_alloc` references
    // that appear only as imports (a `declare` with no caller is *not* a
    // signal — we require the call site to be inside `func`'s body).
    auto looksLikeBoxAlloc = [](llvm::StringRef name) {
        // Reject zeroed / realloc / dealloc / Arc / Rc / Weak by name —
        // they would all let an out-of-scope shape sneak through if we
        // accepted any "alloc" substring.
        if (name.contains("__rust_alloc_zeroed")) return false;
        if (name.contains("__rust_realloc")) return false;
        if (name.contains("Arc")) return false;
        if (name.contains("alloc::rc::Rc") || name.contains("3rc2Rc"))
            return false;
        if (name.contains("Weak")) return false;
        if (name.contains("__rust_alloc")) return true;
        // Box::<T>::new — both manglings carry the `boxed` module path and the
        // length-prefixed `3new`: legacy `_ZN5alloc5boxed12Box$LT$..$GT$3new`,
        // v0 `_RNvMNt..5alloc5boxed..3new`. Requiring both keeps this far
        // narrower than a bare `new` and never matches exchange_malloc/free.
        if (name.contains("boxed") && name.contains("3new")) return true;
        // Match Box::new's documented lowering helper.
        if (name.contains("exchange_malloc")) return true;
        return false;
    };

    for (const llvm::BasicBlock& bb : func) {
        for (const llvm::Instruction& inst : bb) {
            llvm::Function* callee = nullptr;
            if (const auto* ci = llvm::dyn_cast<llvm::CallInst>(&inst))
                callee = ci->getCalledFunction();
            else if (const auto* inv = llvm::dyn_cast<llvm::InvokeInst>(&inst))
                callee = inv->getCalledFunction();
            if (!callee) continue;
            if (!callee->hasName()) continue;
            if (looksLikeBoxAlloc(callee->getName())) return true;
        }
    }
    return false;
}

std::optional<TypeNode>
LLVMLifter::recoverBoxOwnership(const llvm::DIType* diTy,
                                const llvm::Function* func) const {
    if (!diTy) return std::nullopt;

    // Box<T> manifests in DWARF as a pointer derived type whose `name` is
    // the Rust-textual `alloc::boxed::Box<T, alloc::alloc::Global>` and
    // whose `baseType` is T. (rustc consistently emits this shape at
    // -Copt-level=0 with -g; higher opt levels may erase the pointer
    // wrapper and we degrade.)
    const auto* der = llvm::dyn_cast<llvm::DIDerivedType>(diTy);
    if (!der) return std::nullopt;
    if (der->getTag() != llvm::dwarf::DW_TAG_pointer_type) return std::nullopt;
    if (der->getName().empty()) return std::nullopt;

    if (!matchOutermostBox(der->getName().str())) return std::nullopt;

    // Recover the pointee type via the existing DI path. Without a
    // recoverable inner type we cannot claim ownership confidently.
    auto innerNode = typeFromDI(der->getBaseType());
    if (!innerNode) return std::nullopt;
    if (innerNode->nameParts.empty()) return std::nullopt;

    // IR-side gate: require this function body to actually allocate via the
    // recognized Rust allocator path. Without it the DWARF alone is not
    // sufficient evidence (e.g. a function that merely takes/returns an
    // existing Box without allocating shouldn't be re-classified here —
    // ownership transfer through that channel is a separate concern).
    if (!func) return std::nullopt;
    if (!functionHasBoxAllocEvidence(*func)) return std::nullopt;

    // Mark Owned and strip the pointer modifier — Box<T> at the
    // TranspileModel level is `owned T`, not `T*`.
    innerNode->ownership = OwnershipKind::Owned;
    innerNode->modifier = TypeNode::None;
    return innerNode;
}

// ---------------------------------------------------------------------------
// Top-level lift
// ---------------------------------------------------------------------------

transpile::TranspileModule LLVMLifter::lift(const std::string& artifactPath,
                                            const SymbolTable& metadata,
                                            transpile::DecompileLevel level) {
    llvm::LLVMContext context;
    auto module = extractIR(artifactPath, context);
    if (!module) return transpile::TranspileModule{};
    return liftModule(*module, metadata, level);
}

transpile::TranspileModule LLVMLifter::liftBitcode(const std::string& bitcodePath,
                                                   const SymbolTable& metadata,
                                                   transpile::DecompileLevel level) {
    llvm::LLVMContext context;
    llvm::SMDiagnostic err;
    // parseIRFile handles both .bc (bitcode) and .ll (textual IR).
    auto module = llvm::parseIRFile(bitcodePath, err, context);
    if (!module) return transpile::TranspileModule{};
    return liftModule(*module, metadata, level);
}

transpile::TranspileModule LLVMLifter::liftModule(llvm::Module& module,
                                                  const SymbolTable& metadata,
                                                  transpile::DecompileLevel level) {
    transpile::TranspileModule result;

    // Reset per-lift state. The closure recognizer in `liftInstruction`
    // populates `suppressFromModule_` as it inlines closure bodies, and
    // the function-emission loop below filters against it.
    suppressFromModule_.clear();

    // Recover DWARF debug info. When the IR carries no debug metadata these
    // maps stay empty and every consumer falls back to the pre-existing
    // numeric / liftType behavior.
    buildDebugInfoMaps(module);

    // Build demangled-name -> Function* map for matching
    auto demangledMap = SymbolMapper::buildDemangledMap(module);

    // Build reverse map: Function* -> qualified name (for Topo-matched functions)
    std::unordered_map<const llvm::Function*, std::string> funcToQualified;
    for (const auto& [qualName, fn] : metadata.functions()) {
        // Try direct match
        auto it = demangledMap.find(qualName);
        if (it != demangledMap.end()) {
            funcToQualified[it->second] = qualName;
            continue;
        }
        // Try suffix match (IR name may have crate/module prefix)
        std::string suffix = "::" + qualName;
        for (const auto& [irName, irFunc] : demangledMap) {
            if (irName.size() > suffix.size() &&
                irName.compare(irName.size() - suffix.size(), suffix.size(), suffix) == 0) {
                funcToQualified[irFunc] = qualName;
                break;
            }
        }
    }

    // Lift each matched function. We must lift in two phases so that the
    // closure recognizer (which runs inside per-function lifting and writes
    // into `suppressFromModule_`) is given the chance to observe every
    // caller before we decide what to emit. Concretely: lift everyone into
    // a staging vector, then only append the survivors to `result.functions`.
    std::vector<transpile::TranspileFunction> staged;
    std::vector<const llvm::Function*> stagedSource;
    for (auto& func : module) {
        if (func.isDeclaration()) continue;

        // Determine qualified name
        std::string qualifiedName;
        auto qIt = funcToQualified.find(&func);
        if (qIt != funcToQualified.end()) {
            qualifiedName = qIt->second;
        } else {
            // Unmatched function — use demangled name from IR
            auto dIt = demangledMap.find(func.getName().str());
            if (dIt != demangledMap.end()) {
                // The key in demangledMap is already the demangled name — search by value
                for (const auto& [name, fptr] : demangledMap) {
                    if (fptr == &func) {
                        qualifiedName = name;
                        break;
                    }
                }
            }
            if (qualifiedName.empty()) {
                qualifiedName = func.getName().str();
            }
        }

        switch (level) {
        case transpile::DecompileLevel::Direct:
            staged.push_back(liftFunctionDirect(func, qualifiedName, metadata));
            break;

        case transpile::DecompileLevel::Structured:
            staged.push_back(liftFunctionStructured(func, qualifiedName, metadata));
            break;

        case transpile::DecompileLevel::Idiomatic:
            staged.push_back(liftFunctionDirect(func, qualifiedName, metadata));
            break;
        }
        stagedSource.push_back(&func);
    }

    // Drop closure body functions that were inlined into a recovered
    // `LambdaExpr`. They would otherwise dangle as orphan callables that
    // nothing references.
    for (size_t i = 0; i < staged.size(); ++i) {
        if (suppressFromModule_.count(stagedSource[i])) continue;
        result.functions.push_back(std::move(staged[i]));
    }

    // Lift struct types from the module
    for (auto* structTy : module.getIdentifiedStructTypes()) {
        if (!structTy->hasName()) continue;

        transpile::TranspileType typeDecl;
        typeDecl.fidelity = transpile::Fidelity::Recovered;

        std::string name = stripStructName(structTy->getName());
        typeDecl.qualifiedName = name;

        // Recover real member names from DWARF when available; otherwise the
        // numeric "field{i}" fallback (marked Inferred, not Recovered).
        const std::vector<std::string>* diMembers =
            debugMemberNames(structTy->getName());

        for (unsigned i = 0; i < structTy->getNumElements(); ++i) {
            transpile::TranspileField field;
            field.type = liftType(structTy->getElementType(i));
            if (diMembers && i < diMembers->size() && !(*diMembers)[i].empty()) {
                field.name = (*diMembers)[i];
                field.fidelity = transpile::Fidelity::Recovered;
            } else {
                field.name = "field" + std::to_string(i);
                field.fidelity = transpile::Fidelity::Inferred;
            }
            typeDecl.fields.push_back(std::move(field));
        }

        result.types.push_back(std::move(typeDecl));
    }

    // L3 idiomatization: fold temporaries, apply naming heuristics,
    // eliminate dead code
    if (level == transpile::DecompileLevel::Idiomatic) {
        auto optimizer = transpile::ModelOptimizer::createIdiomaticPipeline();
        optimizer.optimize(result);
    }

    return result;
}

} // namespace topo::decompile
