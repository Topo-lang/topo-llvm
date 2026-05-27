// Reusable pass-event runtime wire.
//
// Owns the NDJSON-emitting helper for "a Pass changed an observable
// runtime decision" events. It deliberately reuses the same one-record-
// per-stdout-line exporter discipline as topo_observe.cpp (lock + write
// + the caller controls flushing) rather than duplicating an exporter
// abstraction: the only shared resource is stdout line framing, and
// keeping a single tiny TU lets the linker dead-strip it when no Pass
// injected a call.
//
// Placement rationale: the variant-switch *knowledge*
// lives in the adaptive runtime (it owns the jitPtr atomic store), but
// the *NDJSON framing* is pass-agnostic and must be shareable by
// LifetimeArena/TopoParallel later. So the framing lives here in its
// own libtopo-pass-event; topo-adaptive PUBLIC-links it and calls
// topo_pass_event_emit() from the actual switch sites.

#include "topo/rt/pass_event_rt.h"

#include "json_escape.h"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace {

// Serializes pass-event writes against each other. libtopo-observe uses
// its own mutex for spans; the two streams are line-framed so OS-level
// stdout interleaving stays record-aligned even without a shared lock.
std::mutex g_passEventMutex;

} // namespace

namespace {

// Shared shaper for both entry points. `hasBytes==false` reproduces the
// exact pre-existing topo_pass_event_emit() byte layout (no "bytes"
// key), guaranteeing AdaptiveDispatch output is unchanged. When
// `hasBytes==true` a `"bytes":<int64>` key is inserted right before
// `ts_ns` (matching the schema order documented in pass_event_rt.h).
//
// All string fields are routed through topo::rt::writeJsonString so
// user-controlled names (pipeline / function / lifetime-scope) cannot
// inject raw quotes / backslashes / control chars that would break
// topo-profile's record-aligned framing.
void emitRecord(const char* pass, const char* from, const char* to,
                const char* subject, bool hasBytes, int64_t bytes) {
    auto now = std::chrono::system_clock::now();
    auto epochNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       now.time_since_epoch())
                       .count();

    std::lock_guard<std::mutex> lock(g_passEventMutex);

    // `subject` is omitted entirely when empty — mirrors topo-profile's
    // "omit rather than invent" convention so consumers never see a
    // synthetic placeholder value.
    const bool hasSubject = subject && subject[0] != '\0';

    std::fputs("{\"kind\":\"pass_event\",\"pass\":", stdout);
    topo::rt::writeJsonString(stdout, pass);
    std::fputs(",\"from\":", stdout);
    topo::rt::writeJsonString(stdout, from);
    std::fputs(",\"to\":", stdout);
    topo::rt::writeJsonString(stdout, to);
    if (hasSubject) {
        std::fputs(",\"subject\":", stdout);
        topo::rt::writeJsonString(stdout, subject);
    }
    if (hasBytes) {
        std::fprintf(stdout, ",\"bytes\":%lld",
                     static_cast<long long>(bytes));
    }
    std::fprintf(stdout, ",\"ts_ns\":%lld}\n",
                 static_cast<long long>(epochNs));

    // Flush so the record is visible to a draining parent even if the
    // process is later killed (matches libtopo-observe's per-shutdown
    // flush contract; we flush per-record because pass events are rare).
    std::fflush(stdout);
}

} // namespace

extern "C" uint32_t topo_pass_event_version(void) {
    return TOPO_PASS_EVENT_ABI_VERSION;
}

extern "C" void topo_pass_event_emit(const char* pass,
                                     const char* from,
                                     const char* to,
                                     const char* subject) {
    emitRecord(pass, from, to, subject, /*hasBytes=*/false, 0);
}

extern "C" void topo_pass_event_emit_sized(const char* pass,
                                           const char* from,
                                           const char* to,
                                           const char* subject,
                                           int64_t bytes) {
    emitRecord(pass, from, to, subject, /*hasBytes=*/true, bytes);
}
