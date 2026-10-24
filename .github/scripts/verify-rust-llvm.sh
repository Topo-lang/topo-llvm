#!/bin/sh
# Check the actual Rust producer and LLVM reader before building dependencies.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <llvm-bin-directory>" >&2
    exit 2
fi
llvm_bin=$1
rust_version=$(rustc --version --verbose)
reader_version=$("$llvm_bin/llvm-config" --version)
printf '%s\n' "$rust_version"
cargo --version
printf 'LLVM reader version: %s\n' "$reader_version"
producer_major=$(printf '%s\n' "$rust_version" | awk '/^LLVM version:/ {split($3, v, "."); print v[1]}')
reader_major=${reader_version%%.*}
case "$producer_major:$reader_major" in
    *[!0-9:]*|:*|*:)
        echo "error: could not identify Rust producer and LLVM reader major versions" >&2
        exit 1
        ;;
esac
if [ "$producer_major" != "$reader_major" ]; then
    echo "error: Rust bitcode producer LLVM $producer_major does not match reader $reader_version" >&2
    exit 1
fi

# Matching version strings alone do not prove bitcode compatibility. Compile
# with the selected rustc and make the selected LLVM reader parse its output.
probe_dir=$(mktemp -d)
trap 'rm -rf "$probe_dir"' EXIT HUP INT TERM
cat > "$probe_dir/probe.rs" <<'RUST'
#![no_std]
#[no_mangle]
pub extern "C" fn topo_rust_llvm_probe(a: u32, b: u32) -> u32 {
    a.wrapping_add(b)
}
RUST
rustc --crate-type lib --emit llvm-bc -C opt-level=1 -C panic=abort \
    "$probe_dir/probe.rs" -o "$probe_dir/probe.bc"
"$llvm_bin/llvm-dis" "$probe_dir/probe.bc" -o "$probe_dir/probe.ll"
if ! grep -q 'define .*@topo_rust_llvm_probe(' "$probe_dir/probe.ll"; then
    echo "error: LLVM reader did not recover the Rust probe function" >&2
    exit 1
fi
echo "Rust bitcode compiled and read successfully (LLVM $reader_version)"
