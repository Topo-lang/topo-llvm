#include "E2eHarness.h"

#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"

#include <cstdlib>
#include <filesystem>
#include <string>

// Rust impl-method IR verification lane.
//
// Regression fixture for the impl-method symbol-mapping gap: rustc v0-mangles
// `impl Cart { pub fn new(...) }` as an _RNvMC... symbol that demangles to
// "<rust_impl_methods::Cart>::new" — an angle-bracket impl wrapper the
// SymbolMapper never folded back onto the ".topo" "ns::Type::method" key, so
// topo-build IR verification failed with "missing in LLVM IR" for every
// declared struct method (free functions were unaffected — their demangles
// are plain "crate::module::fn" paths). The fixture declares and implements
// two methods (new + total); this lane pins that the full topo-build flow
// now maps both and verifies cleanly.

namespace topo::test::e2e {

namespace {
namespace fs = std::filesystem;

// Recursively copy the source fixture tree into `dst` (removing any prior
// copy). Each test gets a fresh private working copy so cargo's target/ and
// topo's build state never collide under `ctest -j N`.
void copyFixtureTree(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(src, dst, fs::copy_options::recursive, ec);
}
} // namespace

class RustImplMethod : public E2eFixture {
protected:
    fs::path fixtureSrcDir() const {
#ifdef TOPO_E2E_RUST_IMPL_FIXTURE_DIR
        return fs::path(TOPO_E2E_RUST_IMPL_FIXTURE_DIR);
#else
        return {};
#endif
    }

    void SetUp() override {
        E2eFixture::SetUp();
        fs::path src = fixtureSrcDir();
        ASSERT_FALSE(src.empty()) << "TOPO_E2E_RUST_IMPL_FIXTURE_DIR not defined";
        ASSERT_TRUE(fs::exists(src)) << "rust_impl_methods fixture not found: " << src;
        workDir_ = fs::temp_directory_path() / "topo_e2e_rust_impl_methods";
        copyFixtureTree(src, workDir_);
        projectsDir_ = workDir_.parent_path();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(workDir_, ec);
    }

    fs::path workDir_;
};

TEST_F(RustImplMethod, StructImplMethodsPassIRVerification) {
#ifdef _WIN32
    int ret = std::system("cargo --version > NUL 2>&1");
#else
    int ret = std::system("cargo --version > /dev/null 2>&1");
#endif
    if (ret != 0) {
        GTEST_SKIP() << "cargo not available, skipping StructImplMethodsPassIRVerification";
    }

    auto build = topoBuild("rust_impl_methods");
    ASSERT_EQ(build.exitCode, 0) << "topo-build failed (impl methods must verify against the "
                                    "angle-bracketed rust v0 demangles):\n"
                                 << build.output;

    // lib crate — no binary to run; a clean Step-5 verification (the
    // assertion above) is the contract this fixture pins.
}

} // namespace topo::test::e2e
