// Schema round-trip coverage for the per-Pass sidecar serializer
// (`writePassReportsSidecar`). Closes a coverage gap: the
// existing per-Pass unit tests only assert the in-memory report struct is
// populated — they never assert the *serialized* JSON schema. Each test here
// constructs a non-trivial report, serializes it through the real
// `PassReportsSidecar` API into a temp directory, reads the produced
// `<...>.topo-passes/<PassName>.json` back, and asserts every documented
// field round-trips by value (header + per-Pass detail).

#include "topo/Backend/PassReports.h"
#include "topo/Backend/PassReportsSidecar.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace topo::backend;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Fresh unique temp dir per test; auto-removed on destruction.
class SidecarScratch {
public:
    SidecarScratch() {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path() /
               ("topo-passes-schema-" + std::to_string(stamp) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir_);
    }
    ~SidecarScratch() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    // The outputPath given to the serializer; sidecar dir is this + ".topo-passes".
    std::string outputPath() const { return (dir_ / "bin").string(); }

    json readPass(const std::string& passName) const {
        fs::path p = fs::path(outputPath() + ".topo-passes") / (passName + ".json");
        EXPECT_TRUE(fs::exists(p)) << "missing sidecar file: " << p.string();
        std::ifstream in(p, std::ios::binary);
        EXPECT_TRUE(in.good()) << "cannot open: " << p.string();
        json j;
        in >> j;
        return j;
    }

private:
    fs::path dir_;
};

// Common header assertions shared by every per-Pass test.
void assertHeader(const json& j, const std::string& pass, const std::string& category,
                  bool fired, int firedCount, const std::string& decision,
                  const std::string& reason, int64_t elapsedNs) {
    ASSERT_TRUE(j.is_object());
    ASSERT_TRUE(j.contains("header"));
    const json& h = j.at("header");
    ASSERT_TRUE(h.is_object());

    ASSERT_TRUE(h.contains("pass"));
    EXPECT_TRUE(h.at("pass").is_string());
    EXPECT_EQ(h.at("pass").get<std::string>(), pass);

    ASSERT_TRUE(h.contains("category"));
    EXPECT_EQ(h.at("category").get<std::string>(), category);

    ASSERT_TRUE(h.contains("fired"));
    EXPECT_TRUE(h.at("fired").is_boolean());
    EXPECT_EQ(h.at("fired").get<bool>(), fired);

    ASSERT_TRUE(h.contains("fired_count"));
    EXPECT_TRUE(h.at("fired_count").is_number_integer());
    EXPECT_EQ(h.at("fired_count").get<int>(), firedCount);

    ASSERT_TRUE(h.contains("decision"));
    EXPECT_EQ(h.at("decision").get<std::string>(), decision);

    ASSERT_TRUE(h.contains("reason"));
    EXPECT_EQ(h.at("reason").get<std::string>(), reason);

    ASSERT_TRUE(h.contains("elapsed_ns"));
    EXPECT_TRUE(h.at("elapsed_ns").is_number_integer());
    EXPECT_EQ(h.at("elapsed_ns").get<int64_t>(), elapsedNs);
}

// ---------------------------------------------------------------------------
// DataLayoutPass — header + candidates[] (richest numeric/string detail).
// ---------------------------------------------------------------------------
TEST(PassReportsSidecarSchemaTest, DataLayoutPassSchemaRoundTrips) {
    SidecarScratch scratch;
    PassReports reports;

    auto& r = reports.dataLayout;
    r.header.passName = "DataLayoutPass";
    r.header.category = "OPT";
    r.header.fired = true;
    r.header.firedCount = 2;
    r.header.decision = "auto_soa";
    r.header.reason = "SoA variant 1.83x faster on Particles[128]";
    r.header.elapsedNs = 123456789;

    DataLayoutCandidate c;
    c.pipelineName = "simulate";
    c.wrapperIRType = "struct.topo::array<Particle, 128>";
    c.elementIRType = "struct.Particle";
    c.arraySize = 128;
    c.baselineNs = 9100;
    c.variantNs = 4972;
    c.speedup = 1.83;
    c.winner = "soa";
    c.applied = true;
    r.candidates.push_back(c);

    ASSERT_TRUE(writePassReportsSidecar(reports, scratch.outputPath()));
    json j = scratch.readPass("DataLayoutPass");

    assertHeader(j, "DataLayoutPass", "OPT", true, 2, "auto_soa",
                 "SoA variant 1.83x faster on Particles[128]", 123456789);

    ASSERT_TRUE(j.contains("candidates"));
    const json& arr = j.at("candidates");
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 1u);
    const json& o = arr.at(0);
    EXPECT_EQ(o.at("pipeline").get<std::string>(), "simulate");
    EXPECT_EQ(o.at("wrapper_ir_type").get<std::string>(),
              "struct.topo::array<Particle, 128>");
    EXPECT_EQ(o.at("element_ir_type").get<std::string>(), "struct.Particle");
    EXPECT_TRUE(o.at("array_size").is_number());
    EXPECT_EQ(o.at("array_size").get<uint64_t>(), 128u);
    EXPECT_EQ(o.at("baseline_ns").get<int64_t>(), 9100);
    EXPECT_EQ(o.at("variant_ns").get<int64_t>(), 4972);
    EXPECT_TRUE(o.at("speedup").is_number());
    EXPECT_DOUBLE_EQ(o.at("speedup").get<double>(), 1.83);
    EXPECT_EQ(o.at("winner").get<std::string>(), "soa");
    EXPECT_TRUE(o.at("applied").is_boolean());
    EXPECT_EQ(o.at("applied").get<bool>(), true);
}

// Parallel coverage: topo::array is a `using` alias for std::array, so the
// IR-mangled wrapper name DataLayoutPass records into wrapperIRType can take
// any of three forms depending on the host stdlib. The sidecar JSON must
// round-trip all of them as opaque strings — there is no schema-level reason
// to treat libstdc++ / libc++ names specially.
TEST(PassReportsSidecarSchemaTest, DataLayoutWrapperIRTypeStdArrayRoundTrips) {
    for (const std::string& wrapperName : {
             std::string("struct.topo::array<Particle, 128>"),
             std::string("struct.std::array<Particle, 128>"),
             std::string("struct.std::__1::array<Particle, 128>"),
         }) {
        SidecarScratch scratch;
        PassReports reports;
        auto& r = reports.dataLayout;
        r.header.passName = "DataLayoutPass";
        r.header.category = "OPT";
        r.header.fired = true;
        r.header.firedCount = 1;
        r.header.decision = "forced_soa";
        r.header.reason = "forced";
        r.header.elapsedNs = 1;
        DataLayoutCandidate c;
        c.pipelineName = "simulate";
        c.wrapperIRType = wrapperName;
        c.elementIRType = "struct.Particle";
        c.arraySize = 128;
        c.winner = "forced_soa";
        c.applied = true;
        r.candidates.push_back(c);

        ASSERT_TRUE(writePassReportsSidecar(reports, scratch.outputPath()));
        json j = scratch.readPass("DataLayoutPass");
        ASSERT_TRUE(j.contains("candidates"));
        ASSERT_EQ(j.at("candidates").size(), 1u);
        EXPECT_EQ(j.at("candidates").at(0).at("wrapper_ir_type").get<std::string>(),
                  wrapperName);
    }
}

// ---------------------------------------------------------------------------
// IndirectionPass — header + flat stats object (8 integer counters).
// Edge: Pass did not fire ⇒ fired:false and all counters serialize as 0.
// ---------------------------------------------------------------------------
TEST(PassReportsSidecarSchemaTest, IndirectionPassSchemaRoundTrips) {
    SidecarScratch scratch;
    PassReports reports;

    auto& r = reports.indirection;
    r.header.passName = "IndirectionPass";
    r.header.category = "OPT";
    r.header.fired = true;
    r.header.firedCount = 7;
    r.header.decision = "applied";
    r.header.reason = "8 indirection optimisations scored";
    r.header.elapsedNs = 42;
    r.uniquePtrPromoted = 3;
    r.sharedPtrOptimized = 5;
    r.sharedPtrDereferenced = 1;
    r.refcountEliminated = 9;
    r.vectorLowered = 2;
    r.pointerAttrsAdded = 11;
    r.callsDevirtualized = 4;
    r.vtableConstantsAnnotated = 6;

    ASSERT_TRUE(writePassReportsSidecar(reports, scratch.outputPath()));
    json j = scratch.readPass("IndirectionPass");

    assertHeader(j, "IndirectionPass", "OPT", true, 7, "applied",
                 "8 indirection optimisations scored", 42);

    ASSERT_TRUE(j.contains("stats"));
    const json& s = j.at("stats");
    ASSERT_TRUE(s.is_object());
    EXPECT_TRUE(s.at("unique_ptr_promoted").is_number_integer());
    EXPECT_EQ(s.at("unique_ptr_promoted").get<int>(), 3);
    EXPECT_EQ(s.at("shared_ptr_optimized").get<int>(), 5);
    EXPECT_EQ(s.at("shared_ptr_dereferenced").get<int>(), 1);
    EXPECT_EQ(s.at("refcount_eliminated").get<int>(), 9);
    EXPECT_EQ(s.at("vector_lowered").get<int>(), 2);
    EXPECT_EQ(s.at("pointer_attrs_added").get<int>(), 11);
    EXPECT_EQ(s.at("calls_devirtualized").get<int>(), 4);
    EXPECT_EQ(s.at("vtable_constants_annotated").get<int>(), 6);

    // Negative/edge: a never-fired IndirectionReport still serializes a full
    // object with fired:false and zeroed counters (not missing / not null).
    SidecarScratch edge;
    PassReports zeroReports;
    auto& z = zeroReports.indirection;
    z.header.passName = "IndirectionPass";
    z.header.category = "OPT";
    z.header.fired = false;
    z.header.firedCount = 0;
    z.header.decision = "disabled";
    z.header.reason = "indirection optimisations off";
    ASSERT_TRUE(writePassReportsSidecar(zeroReports, edge.outputPath()));
    json zj = edge.readPass("IndirectionPass");
    assertHeader(zj, "IndirectionPass", "OPT", false, 0, "disabled",
                 "indirection optimisations off", 0);
    ASSERT_TRUE(zj.contains("stats"));
    ASSERT_TRUE(zj.at("stats").is_object());
    EXPECT_FALSE(zj.at("stats").at("unique_ptr_promoted").is_null());
    EXPECT_EQ(zj.at("stats").at("unique_ptr_promoted").get<int>(), 0);
    EXPECT_EQ(zj.at("stats").at("vtable_constants_annotated").get<int>(), 0);
}

// ---------------------------------------------------------------------------
// ReturnSpecializationPass — header + entries[] with integer-array members.
// Edge: empty entries[] still serializes as [] (not missing / not null).
// ---------------------------------------------------------------------------
TEST(PassReportsSidecarSchemaTest, ReturnSpecializationPassSchemaRoundTrips) {
    SidecarScratch scratch;
    PassReports reports;

    auto& r = reports.returnSpecialization;
    r.header.passName = "ReturnSpecializationPass";
    r.header.category = "OPT";
    r.header.fired = true;
    r.header.firedCount = 1;
    r.header.decision = "specialized";
    r.header.reason = "2 fields unused in detectResult";
    r.header.elapsedNs = 777;

    ReturnSpecializationEntry e;
    e.hostFunction = "detectResult";
    e.eliminatedFieldIndices = {1, 3};
    e.keptFieldIndices = {0, 2, 4};
    r.entries.push_back(e);

    ASSERT_TRUE(writePassReportsSidecar(reports, scratch.outputPath()));
    json j = scratch.readPass("ReturnSpecializationPass");

    assertHeader(j, "ReturnSpecializationPass", "OPT", true, 1, "specialized",
                 "2 fields unused in detectResult", 777);

    ASSERT_TRUE(j.contains("entries"));
    const json& arr = j.at("entries");
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 1u);
    const json& o = arr.at(0);
    EXPECT_EQ(o.at("host_function").get<std::string>(), "detectResult");
    ASSERT_TRUE(o.at("eliminated_field_indices").is_array());
    EXPECT_EQ(o.at("eliminated_field_indices").get<std::vector<int>>(),
              (std::vector<int>{1, 3}));
    ASSERT_TRUE(o.at("kept_field_indices").is_array());
    EXPECT_EQ(o.at("kept_field_indices").get<std::vector<int>>(),
              (std::vector<int>{0, 2, 4}));

    // Negative/edge: no eliminations ⇒ entries serializes as [] not null/missing.
    SidecarScratch edge;
    PassReports emptyReports;
    auto& z = emptyReports.returnSpecialization;
    z.header.passName = "ReturnSpecializationPass";
    z.header.category = "OPT";
    z.header.fired = false;
    z.header.firedCount = 0;
    z.header.decision = "no_candidates";
    z.header.reason = "no sret functions with dead fields";
    ASSERT_TRUE(writePassReportsSidecar(emptyReports, edge.outputPath()));
    json zj = edge.readPass("ReturnSpecializationPass");
    assertHeader(zj, "ReturnSpecializationPass", "OPT", false, 0, "no_candidates",
                 "no sret functions with dead fields", 0);
    ASSERT_TRUE(zj.contains("entries"));
    ASSERT_TRUE(zj.at("entries").is_array());
    EXPECT_EQ(zj.at("entries").size(), 0u);
}

// ---------------------------------------------------------------------------
// TopoInlinePass — header + entries[] of {callee, reason}.
// ---------------------------------------------------------------------------
TEST(PassReportsSidecarSchemaTest, TopoInlinePassSchemaRoundTrips) {
    SidecarScratch scratch;
    PassReports reports;

    auto& r = reports.topoInline;
    r.header.passName = "TopoInlinePass";
    r.header.category = "OPT";
    r.header.fired = true;
    r.header.firedCount = 2;
    r.header.decision = "inlined";
    r.header.reason = "visibility + pipeline functor inlining";
    r.header.elapsedNs = 5150;

    TopoInlineEntry e1{"helperPrivate", "private"};
    TopoInlineEntry e2{"stageFunctor", "pipeline_functor"};
    r.entries.push_back(e1);
    r.entries.push_back(e2);

    ASSERT_TRUE(writePassReportsSidecar(reports, scratch.outputPath()));
    json j = scratch.readPass("TopoInlinePass");

    assertHeader(j, "TopoInlinePass", "OPT", true, 2, "inlined",
                 "visibility + pipeline functor inlining", 5150);

    ASSERT_TRUE(j.contains("entries"));
    const json& arr = j.at("entries");
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 2u);
    EXPECT_EQ(arr.at(0).at("callee").get<std::string>(), "helperPrivate");
    EXPECT_EQ(arr.at(0).at("reason").get<std::string>(), "private");
    EXPECT_EQ(arr.at(1).at("callee").get<std::string>(), "stageFunctor");
    EXPECT_EQ(arr.at(1).at("reason").get<std::string>(), "pipeline_functor");
}

// ---------------------------------------------------------------------------
// TopoParallelPass — header + candidates[] (benchmark numerics).
// Edge: a serial-winner candidate with applied:false (Pass fired but did not
// apply parallelism) still round-trips every field by value.
// ---------------------------------------------------------------------------
TEST(PassReportsSidecarSchemaTest, TopoParallelPassSchemaRoundTrips) {
    SidecarScratch scratch;
    PassReports reports;

    auto& r = reports.topoParallel;
    r.header.passName = "TopoParallelPass";
    r.header.category = "OPT";
    r.header.fired = true;
    r.header.firedCount = 2;
    r.header.decision = "auto_mixed";
    r.header.reason = "1 pipeline parallelized, 1 kept serial";
    r.header.elapsedNs = 998877;

    TopoParallelCandidate win;
    win.pipelineName = "renderFrame";
    win.baselineNs = 30000;
    win.variantNs = 11100;
    win.speedup = 2.70;
    win.winner = "parallel";
    win.applied = true;
    r.candidates.push_back(win);

    TopoParallelCandidate lose;
    lose.pipelineName = "tinyStage";
    lose.baselineNs = 800;
    lose.variantNs = 1500;
    lose.speedup = 0.53;
    lose.winner = "serial";
    lose.applied = false;
    r.candidates.push_back(lose);

    ASSERT_TRUE(writePassReportsSidecar(reports, scratch.outputPath()));
    json j = scratch.readPass("TopoParallelPass");

    assertHeader(j, "TopoParallelPass", "OPT", true, 2, "auto_mixed",
                 "1 pipeline parallelized, 1 kept serial", 998877);

    ASSERT_TRUE(j.contains("candidates"));
    const json& arr = j.at("candidates");
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 2u);

    const json& a = arr.at(0);
    EXPECT_EQ(a.at("pipeline").get<std::string>(), "renderFrame");
    EXPECT_EQ(a.at("baseline_ns").get<int64_t>(), 30000);
    EXPECT_EQ(a.at("variant_ns").get<int64_t>(), 11100);
    EXPECT_TRUE(a.at("speedup").is_number());
    EXPECT_DOUBLE_EQ(a.at("speedup").get<double>(), 2.70);
    EXPECT_EQ(a.at("winner").get<std::string>(), "parallel");
    EXPECT_EQ(a.at("applied").get<bool>(), true);

    const json& b = arr.at(1);
    EXPECT_EQ(b.at("pipeline").get<std::string>(), "tinyStage");
    EXPECT_EQ(b.at("baseline_ns").get<int64_t>(), 800);
    EXPECT_EQ(b.at("variant_ns").get<int64_t>(), 1500);
    EXPECT_DOUBLE_EQ(b.at("speedup").get<double>(), 0.53);
    EXPECT_EQ(b.at("winner").get<std::string>(), "serial");
    EXPECT_TRUE(b.at("applied").is_boolean());
    EXPECT_EQ(b.at("applied").get<bool>(), false);
}

} // namespace
