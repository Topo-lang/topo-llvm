#!/usr/bin/env bash
# Download and extract pre-built LLVM into topo-llvm/llvm-dev/
# Reads version from topo-llvm/.llvm-version.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOPO_LLVM_ROOT="$(dirname "$SCRIPT_DIR")"

VERSION_FILE="$TOPO_LLVM_ROOT/.llvm-version"
if [ ! -f "$VERSION_FILE" ]; then
    echo "error: $VERSION_FILE not found" >&2
    exit 1
fi
VER=$(tr -d '[:space:]' < "$VERSION_FILE")
echo "LLVM version: $VER"

CHECKSUMS_FILE="$TOPO_LLVM_ROOT/.llvm-checksums"

# Print the lowercase hex SHA-256 of "$1", using whatever tool is present
# (sha256sum on Linux/Git-Bash, shasum on macOS, openssl as a last resort).
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    else
        echo "NO_SHA256_TOOL"
    fi
}

LLVM_DIR="$TOPO_LLVM_ROOT/llvm-dev"

# Check existing installation
if [ -f "$LLVM_DIR/lib/cmake/llvm/LLVMConfig.cmake" ]; then
    EXISTING=$("$LLVM_DIR/bin/llvm-config" --version 2>/dev/null || echo "")
    if [ "$EXISTING" = "$VER" ]; then
        echo "llvm-dev/ already contains LLVM $VER — skipping download"
        exit 0
    fi
    echo "Version mismatch (found $EXISTING, want $VER) — re-downloading"
    rm -rf "$LLVM_DIR"
fi

# Detect platform
OS=$(uname -s)
ARCH=$(uname -m)

# Determine major version for naming convention
MAJOR=$(echo "$VER" | cut -d. -f1)

case "$OS" in
    MINGW*|MSYS*|CYGWIN*|*_NT*)
        # Windows — always uses old naming format
        ARCHIVE="clang+llvm-${VER}-x86_64-pc-windows-msvc.tar.xz"
        PLATFORM="windows"
        ;;
    Darwin)
        # macOS: the prebuilt bundled tarball is unverified here and can abort
        # at link time with `dyld: Symbol not found: __ZdaPv` (a libc++ ABI
        # mismatch). Prefer Homebrew llvm@<major>, which cmake/llvm-detect.cmake
        # also picks up preferentially; skip the bundled download when present.
        if command -v brew >/dev/null 2>&1; then
            BREW_LLVM=$(brew --prefix "llvm@${MAJOR}" 2>/dev/null)
            if [ -n "$BREW_LLVM" ] && [ -x "$BREW_LLVM/bin/clang++" ]; then
                echo "Found Homebrew llvm@${MAJOR} at $BREW_LLVM"
                echo "Using it instead of the bundled tarball (recommended on macOS);"
                echo "cmake/llvm-detect.cmake locates it automatically."
                exit 0
            fi
        fi
        echo "warning: no Homebrew llvm@${MAJOR} found; downloading the bundled" >&2
        echo "         macOS LLVM tarball, which is UNVERIFIED on macOS and may" >&2
        echo "         fail at link time with 'dyld: Symbol not found: __ZdaPv'." >&2
        echo "         Recommended fix if that happens: brew install llvm@${MAJOR}" >&2
        if [ "$ARCH" = "arm64" ]; then
            if [ "$MAJOR" -ge 19 ]; then
                ARCHIVE="LLVM-${VER}-macOS-ARM64.tar.xz"
            else
                ARCHIVE="clang+llvm-${VER}-arm64-apple-macos.tar.xz"
            fi
        else
            if [ "$MAJOR" -ge 19 ]; then
                ARCHIVE="LLVM-${VER}-macOS-X64.tar.xz"
            else
                ARCHIVE="clang+llvm-${VER}-x86_64-apple-darwin.tar.xz"
            fi
        fi
        PLATFORM="macos"
        ;;
    Linux)
        if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
            if [ "$MAJOR" -ge 19 ]; then
                ARCHIVE="LLVM-${VER}-Linux-ARM64.tar.xz"
            else
                ARCHIVE="clang+llvm-${VER}-aarch64-linux-gnu.tar.xz"
            fi
        else
            if [ "$MAJOR" -ge 19 ]; then
                ARCHIVE="LLVM-${VER}-Linux-X64.tar.xz"
            else
                ARCHIVE="clang+llvm-${VER}-x86_64-linux-gnu-ubuntu-22.04.tar.xz"
            fi
        fi
        PLATFORM="linux"
        ;;
    *)
        echo "error: unsupported OS '$OS'" >&2
        exit 1
        ;;
esac

URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${VER}/${ARCHIVE}"
echo "Downloading $URL ..."

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

TARBALL="$TMPDIR/$ARCHIVE"
curl -fSL --progress-bar -o "$TARBALL" "$URL"

# Integrity gate: the bundled toolchain compiles and links every C++/Rust
# artifact (and runs in CI), so a swapped release asset or a hijacked redirect
# must never be trusted silently. Verify against the pinned SHA-256 before
# extracting. See .llvm-checksums for how to regenerate the digests.
EXPECTED_SHA=""
if [ -f "$CHECKSUMS_FILE" ]; then
    EXPECTED_SHA=$(awk -v f="$ARCHIVE" '$2 == f {print $1}' "$CHECKSUMS_FILE")
fi
if [ -n "$EXPECTED_SHA" ]; then
    ACTUAL_SHA=$(sha256_of "$TARBALL")
    if [ "$ACTUAL_SHA" = "NO_SHA256_TOOL" ]; then
        echo "error: no sha256 tool (sha256sum/shasum/openssl) available to" >&2
        echo "       verify $ARCHIVE; refusing to extract an unverified toolchain." >&2
        exit 1
    fi
    if [ "$ACTUAL_SHA" = "$EXPECTED_SHA" ]; then
        echo "SHA-256 verified: $ACTUAL_SHA"
    else
        echo "error: SHA-256 mismatch for $ARCHIVE — refusing to extract." >&2
        echo "  expected: $EXPECTED_SHA" >&2
        echo "  actual:   $ACTUAL_SHA" >&2
        echo "  The download does not match the pin in .llvm-checksums." >&2
        exit 1
    fi
else
    echo "warning: no pinned SHA-256 for '$ARCHIVE' in .llvm-checksums;" >&2
    echo "         toolchain integrity is NOT verified for this platform." >&2
fi

echo "Extracting to llvm-dev/ ..."
mkdir -p "$LLVM_DIR"

if [ "$PLATFORM" = "windows" ]; then
    # Try tar first; fall back to 7z (two-step: .tar.xz → .tar → extract)
    if tar --strip-components=1 -xJf "$TARBALL" -C "$LLVM_DIR" 2>/dev/null; then
        echo "Extracted with tar"
    elif command -v 7z >/dev/null 2>&1; then
        echo "tar failed, falling back to 7z ..."
        7z x -so "$TARBALL" > "$TMPDIR/llvm.tar"
        7z x -o"$TMPDIR/llvm-extracted" "$TMPDIR/llvm.tar"
        # Move inner directory contents into llvm-dev/
        INNER=$(ls "$TMPDIR/llvm-extracted")
        cp -r "$TMPDIR/llvm-extracted/$INNER/"* "$LLVM_DIR/"
        echo "Extracted with 7z"
    else
        echo "error: tar extraction failed and 7z is not available" >&2
        exit 1
    fi
else
    tar --strip-components=1 -xJf "$TARBALL" -C "$LLVM_DIR"
fi

# macOS: remove quarantine attribute
if [ "$PLATFORM" = "macos" ]; then
    xattr -d com.apple.quarantine "$LLVM_DIR/bin/"* 2>/dev/null || true
fi

# Verify
if [ ! -f "$LLVM_DIR/lib/cmake/llvm/LLVMConfig.cmake" ]; then
    echo "error: LLVMConfig.cmake not found after extraction" >&2
    echo "Contents of llvm-dev/:"
    ls -la "$LLVM_DIR/" 2>/dev/null || true
    exit 1
fi

# Windows: fix hardcoded DIA SDK path in LLVMExports.cmake
# Pre-built LLVM packages bake in the build machine's VS path for diaguids.lib.
# Detect the local DIA SDK and patch the cmake file to match.
if [ "$PLATFORM" = "windows" ]; then
    EXPORTS_FILE="$LLVM_DIR/lib/cmake/llvm/LLVMExports.cmake"
    if grep -q "DIA SDK/lib/amd64/diaguids.lib" "$EXPORTS_FILE" 2>/dev/null; then
        # Find local diaguids.lib
        LOCAL_DIA=""
        for vsdir in "/c/Program Files (x86)/Microsoft Visual Studio"/*/*/ \
                      "/c/Program Files/Microsoft Visual Studio"/*/*/; do
            candidate="${vsdir}DIA SDK/lib/amd64/diaguids.lib"
            if [ -f "$candidate" ]; then
                LOCAL_DIA="$candidate"
                break
            fi
        done
        if [ -n "$LOCAL_DIA" ]; then
            # Convert to Windows-style path for CMake
            WIN_DIA=$(echo "$LOCAL_DIA" | sed 's|^/c/|C:/|; s|^/C/|C:/|')
            echo "Patching DIA SDK path to: $WIN_DIA"
            sed -i "s|[^;\"]*DIA SDK/lib/amd64/diaguids.lib|$WIN_DIA|g" "$EXPORTS_FILE"
        else
            echo "warning: diaguids.lib not found locally, LLVMDebugInfoPDB may fail to link" >&2
        fi
    fi
fi

echo "LLVM $VER installed to topo-llvm/llvm-dev/"
