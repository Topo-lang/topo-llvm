#!/usr/bin/env bash
# mixed-llvm-lang Feasibility Test — C++/Rust IR Merge via LLVM
#
# Validates the core premise: rustc (LLVM 21) bitcode can be parsed,
# merged, optimized, and linked by the project's LLVM 22 toolchain.
#
# 10 checkpoints (0-9): PREREQ → RUST_BC → CPP_LL → BC_PARSE →
#   TRIPLE_CHECK → IR_MERGE → SYMBOLS → OPT → LINK → EXEC

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LLVM_BIN="$(cd "$SCRIPT_DIR/../../llvm-dev/bin" && pwd)"
WORK="$SCRIPT_DIR/_work"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

pass_count=0
fail_count=0
warn_count=0

checkpoint() {
    local num="$1" name="$2"
    printf "${CYAN}[%d] %s${NC} ... " "$num" "$name"
}

pass() {
    printf "${GREEN}PASS${NC}\n"
    pass_count=$((pass_count + 1))
}

fail() {
    printf "${RED}FAIL${NC}\n"
    echo "    $1" >&2
    fail_count=$((fail_count + 1))
}

warn() {
    printf "${YELLOW}WARN${NC} %s\n" "$1"
    warn_count=$((warn_count + 1))
}

cleanup() {
    rm -rf "$WORK"
    # Clean Rust build artifacts
    (cd "$SCRIPT_DIR/rust_lib" && cargo clean 2>/dev/null || true)
}

# ── Setup ──────────────────────────────────────────────────────────
rm -rf "$WORK"
mkdir -p "$WORK"

echo "═══════════════════════════════════════════════════════════"
echo " mixed-llvm-lang Feasibility — C++/Rust IR Merge"
echo "═══════════════════════════════════════════════════════════"
echo ""

# ── [0] PREREQ ─────────────────────────────────────────────────────
checkpoint 0 "PREREQ — tool availability"

missing=""
for tool in clang++ llvm-dis llvm-link opt; do
    if [ ! -x "$LLVM_BIN/$tool" ]; then
        missing="$missing $tool"
    fi
done
if ! command -v rustc &>/dev/null; then
    missing="$missing rustc"
fi
if ! command -v cargo &>/dev/null; then
    missing="$missing cargo"
fi

if [ -n "$missing" ]; then
    fail "missing tools:$missing"
    echo "Aborting — prerequisites not met."
    exit 1
fi
pass

clang_ver=$("$LLVM_BIN/clang++" --version 2>&1 | head -1)
rustc_ver=$(rustc --version 2>&1)
rustc_llvm=$(rustc -vV 2>&1 | grep 'LLVM version' | awk '{print $3}')
echo "    Clang:      $clang_ver"
echo "    rustc:      $rustc_ver"
echo "    rustc LLVM: $rustc_llvm"
echo ""

# ── [1] RUST_BC ────────────────────────────────────────────────────
checkpoint 1 "RUST_BC — compile Rust to bitcode"

# Replicate RustDriver flags: --emit=llvm-bc -Copt-level=1 -Cno-prepopulate-passes
if (cd "$SCRIPT_DIR/rust_lib" && \
    cargo rustc --lib -- --emit=llvm-bc \
        -Csymbol-mangling-version=v0 \
        -Copt-level=1 \
        -Cno-prepopulate-passes \
        2>"$WORK/rust_build.log"); then

    # Find the .bc file in target
    rust_bc=$(find "$SCRIPT_DIR/rust_lib/target" -name "*.bc" -type f 2>/dev/null | head -1)
    if [ -n "$rust_bc" ]; then
        cp "$rust_bc" "$WORK/rust.bc"
        pass
        echo "    BC size: $(wc -c < "$WORK/rust.bc") bytes"
    else
        fail "cargo succeeded but no .bc file found"
    fi
else
    fail "cargo rustc failed — see $WORK/rust_build.log"
    cat "$WORK/rust_build.log" >&2
fi
echo ""

# ── [2] CPP_LL ─────────────────────────────────────────────────────
checkpoint 2 "CPP_LL — compile C++ to text IR"

# Replicate CppDriver flags: -S -emit-llvm -O2 -Xclang -disable-llvm-passes
# macOS: bundled clang++ needs explicit SDK path (same as CppDriver::getMacOSSysrootArgs)
SYSROOT_ARGS=()
if [ "$(uname)" = "Darwin" ]; then
    sdk_path=$(xcrun --show-sdk-path 2>/dev/null || true)
    if [ -n "$sdk_path" ]; then
        SYSROOT_ARGS=(-isysroot "$sdk_path")
    fi
fi

if "$LLVM_BIN/clang++" \
    -std=c++17 -S -emit-llvm -O2 \
    -Xclang -disable-llvm-passes \
    "${SYSROOT_ARGS[@]}" \
    "$SCRIPT_DIR/engine.cpp" \
    -o "$WORK/engine.ll" \
    2>"$WORK/cpp_build.log"; then
    pass
    echo "    IR lines: $(wc -l < "$WORK/engine.ll")"
else
    fail "clang++ failed — see $WORK/cpp_build.log"
    cat "$WORK/cpp_build.log" >&2
fi
echo ""

# ── [3] BC_PARSE ───────────────────────────────────────────────────
checkpoint 3 "BC_PARSE — LLVM 22 parses LLVM 21 bitcode"

if "$LLVM_BIN/llvm-dis" "$WORK/rust.bc" -o "$WORK/rust.ll" 2>"$WORK/bc_parse.log"; then
    pass
    echo "    Disassembled IR lines: $(wc -l < "$WORK/rust.ll")"
else
    fail "llvm-dis failed — LLVM 22 cannot parse rustc bitcode"
    cat "$WORK/bc_parse.log" >&2
fi
echo ""

# ── [4] TRIPLE_CHECK ──────────────────────────────────────────────
checkpoint 4 "TRIPLE_CHECK — target triple compatibility"

cpp_triple=$(grep '^target triple' "$WORK/engine.ll" 2>/dev/null | head -1 | sed 's/.*= *"//' | sed 's/".*//')
rust_triple=$(grep '^target triple' "$WORK/rust.ll" 2>/dev/null | head -1 | sed 's/.*= *"//' | sed 's/".*//')

echo ""
echo "    C++ triple:  ${cpp_triple:-<not found>}"
echo "    Rust triple: ${rust_triple:-<not found>}"

if [ "$cpp_triple" = "$rust_triple" ]; then
    pass
    echo "    Triples match — no patching needed."
else
    warn "triple mismatch (expected on macOS)"
    echo "    Patching Rust IR triple to match C++..."
    if [ -n "$cpp_triple" ] && [ -n "$rust_triple" ]; then
        sed -i.bak "s|target triple = \"$rust_triple\"|target triple = \"$cpp_triple\"|g" "$WORK/rust.ll"
        # Also patch data layout if needed — extract from C++ IR
        cpp_datalayout=$(grep '^target datalayout' "$WORK/engine.ll" | head -1)
        rust_datalayout=$(grep '^target datalayout' "$WORK/rust.ll" | head -1)
        if [ "$cpp_datalayout" != "$rust_datalayout" ] && [ -n "$cpp_datalayout" ]; then
            echo "    Data layouts differ — patching Rust IR..."
            # Replace the entire target datalayout line
            rust_dl_escaped=$(echo "$rust_datalayout" | sed 's/[&/\]/\\&/g')
            cpp_dl_escaped=$(echo "$cpp_datalayout" | sed 's/[&/\]/\\&/g')
            sed -i.bak "s|$rust_dl_escaped|$cpp_dl_escaped|" "$WORK/rust.ll"
        fi
        echo "    Patched successfully."
    fi
fi
echo ""

# ── [5] IR_MERGE ──────────────────────────────────────────────────
checkpoint 5 "IR_MERGE — llvm-link merges C++ + Rust IR"

if "$LLVM_BIN/llvm-link" "$WORK/engine.ll" "$WORK/rust.ll" \
    -o "$WORK/merged.bc" 2>"$WORK/link.log"; then
    pass
    # Also produce readable merged IR
    "$LLVM_BIN/llvm-dis" "$WORK/merged.bc" -o "$WORK/merged.ll" 2>/dev/null
    echo "    Merged IR lines: $(wc -l < "$WORK/merged.ll")"
else
    fail "llvm-link failed"
    cat "$WORK/link.log" >&2
fi
echo ""

# ── [6] SYMBOLS ───────────────────────────────────────────────────
checkpoint 6 "SYMBOLS — merged module contains both-side symbols"

has_rust_add=false
has_main=false

if grep -q '@rust_add' "$WORK/merged.ll" 2>/dev/null; then
    has_rust_add=true
fi
if grep -q '@main' "$WORK/merged.ll" 2>/dev/null; then
    has_main=true
fi

if $has_rust_add && $has_main; then
    pass
    echo "    Found: @rust_add, @main"
    # Check if helper was inlined or preserved
    if grep -q 'helper' "$WORK/merged.ll" 2>/dev/null; then
        echo "    Also found: helper (Rust internal, not yet inlined)"
    fi
else
    fail "missing symbols: rust_add=$has_rust_add main=$has_main"
fi
echo ""

# ── [7] OPT ───────────────────────────────────────────────────────
checkpoint 7 "OPT — O2 optimization passes"

if "$LLVM_BIN/opt" -O2 "$WORK/merged.bc" -o "$WORK/optimized.bc" 2>"$WORK/opt.log"; then
    pass
    "$LLVM_BIN/llvm-dis" "$WORK/optimized.bc" -o "$WORK/optimized.ll" 2>/dev/null
    echo "    Optimized IR lines: $(wc -l < "$WORK/optimized.ll")"
    # Check if helper got inlined
    if ! grep -q 'helper' "$WORK/optimized.ll" 2>/dev/null; then
        echo "    helper() was inlined by O2 (cross-language inlining works!)"
    fi
else
    fail "opt -O2 failed"
    cat "$WORK/opt.log" >&2
fi
echo ""

# ── [8] LINK ──────────────────────────────────────────────────────
checkpoint 8 "LINK — link to executable"

if "$LLVM_BIN/clang++" "${SYSROOT_ARGS[@]}" "$WORK/optimized.bc" -o "$WORK/test_binary" 2>"$WORK/final_link.log"; then
    pass
    echo "    Binary size: $(wc -c < "$WORK/test_binary") bytes"
else
    fail "final link failed"
    cat "$WORK/final_link.log" >&2
fi
echo ""

# ── [9] EXEC ──────────────────────────────────────────────────────
checkpoint 9 "EXEC — run and verify output"

if [ -x "$WORK/test_binary" ]; then
    output=$("$WORK/test_binary" 2>&1) && exit_code=0 || exit_code=$?
    echo "    Output: $output"
    if echo "$output" | grep -q "rust_add(10, 20) = 92"; then
        pass
    else
        fail "unexpected output or exit code $exit_code"
    fi
else
    fail "test_binary not found or not executable"
fi
echo ""

# ── Summary ───────────────────────────────────────────────────────
echo "═══════════════════════════════════════════════════════════"
printf " Results: ${GREEN}%d PASS${NC}" "$pass_count"
if [ "$warn_count" -gt 0 ]; then
    printf ", ${YELLOW}%d WARN${NC}" "$warn_count"
fi
if [ "$fail_count" -gt 0 ]; then
    printf ", ${RED}%d FAIL${NC}" "$fail_count"
fi
echo ""
echo "═══════════════════════════════════════════════════════════"

# Cleanup
cleanup

if [ "$fail_count" -gt 0 ]; then
    echo ""
    echo "mixed-llvm-lang feasibility: BLOCKED — $fail_count checkpoint(s) failed."
    exit 1
else
    echo ""
    echo "mixed-llvm-lang feasibility: CONFIRMED — C++/Rust IR merge is viable."
    exit 0
fi
