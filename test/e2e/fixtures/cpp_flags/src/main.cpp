// cpp_flags — host source that cannot build unless [build.cpp].flags reached
// the compile step. CppFlagsTests.cpp builds this project (Topo.toml carries
// flags = ["-DTOPO_E2E_FLAGS_PROOF"]) and runs the binary: the #error below
// is the compile proof, the exit code the run proof.
#ifndef TOPO_E2E_FLAGS_PROOF
#error "TOPO_E2E_FLAGS_PROOF not defined — [build.cpp].flags did not reach the compile step"
#endif

namespace probe {

int answer() {
    return 42;
}

} // namespace probe

int main() {
    return probe::answer() == 42 ? 0 : 1;
}
