// Source compiled to LLVM bitcode at build time and embedded into
// topo-runtime-tests as a byte array (see test_pipeline_bitcode.cmake).
// JITRealPathTest feeds the bitcode to topo_jit_engine_specialize_bytes,
// expects the engine to locate the demangled symbol, JIT-compile it, and
// return a callable function pointer whose output matches the AOT value.
//
// The namespaced pipeline name ("topotest::pipeline") is what
// topo::jit::specialize_bytes() looks up after demangling; the
// pipeline body is deliberately trivial so the verification stays
// focused on the IR → JIT → atomic-store path rather than on codegen
// correctness for complex code.

namespace topotest {

extern "C" int topotest_pipeline_seed() { return 7; }

int pipeline(int input) {
    int seed = topotest_pipeline_seed();
    int acc = input;
    for (int i = 0; i < 4; ++i) {
        acc = acc * 31 + seed + i;
    }
    return acc;
}

} // namespace topotest
