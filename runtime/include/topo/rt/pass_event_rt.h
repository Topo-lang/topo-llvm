#ifndef TOPO_RT_PASS_EVENT_RT_H
#define TOPO_RT_PASS_EVENT_RT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Pass-event runtime wire
// ============================================================
//
// A reusable, pass-agnostic NDJSON record type for "an optimization
// Pass changed an observable runtime decision" events. AdaptiveDispatch
// is the first producer (AOT⇄JIT variant switches); LifetimeArenaPass
// and TopoParallelPass are intended to reuse this exact wire without
// re-deriving it.
//
// Relationship to libtopo-observe spans: this is a DISTINCT record type
// on the SAME stdout stream that topo-profile drains. Span records carry
// {"name","duration_ns","thread_id","ts_ns"} and never a "kind" field.
// Pass-event records always carry a `"kind":"pass_event"` discriminator
// so topo-profile can unambiguously separate the two without heuristics.
//
// -------- Record schema (one JSON object per stdout line) --------
//
//   {"kind":"pass_event",
//    "pass":"<PassName>",        // emitting LLVM Pass, e.g. "AdaptiveDispatchPass"
//    "from":"<variant>",         // variant in effect before the switch
//    "to":"<variant>",           // variant in effect after the switch
//    "subject":"<name>",         // optional: pipeline / scope this applies to
//    "bytes":<int64>,            // optional: numeric magnitude (e.g. arena size)
//    "ts_ns":<int64>}            // wall-clock ns (system_clock epoch)
//
//   `subject` is omitted entirely when empty (mirrors topo-profile's
//   "omit rather than invent" convention for span stage/pipeline).
//
//   `bytes` (used by LifetimeArenaPass) is OPTIONAL and
//   strictly backward-compatible: it is emitted ONLY by the
//   topo_pass_event_emit_sized() entry point. The original
//   topo_pass_event_emit() never writes a "bytes" key, so
//   AdaptiveDispatchPass output is byte-for-byte identical to before
//   this field existed. LifetimeArenaPass encodes the lifetime scope as
//   `subject`, the open/close moment in from/to ("heap"->"arena" on
//   open, "arena"->"freed" on close) and the arena size in `bytes`
//   (requested capacity at open, bytes-in-use at close).
//
//   topo-profile routes these into a top-level
//     "pass_events": { "<PassName>": [ {ts_ns,from,to,subject?,bytes?}, ... ] }
//   object, alongside (not replacing) "spans"/"sampling". `bytes` is
//   surfaced only when the producer emitted it.
//
// -------- Lifecycle / safety contract --------
//
//   * No init/shutdown handshake is required. A call is always safe.
//   * Output is serialized under an internal mutex and flushed per line
//     so it interleaves cleanly with libtopo-observe span output.
//   * `pass`, `from`, `to`, `subject` may be null; null is treated as
//     an empty string ("" for pass/from/to, omitted for subject).
//   * Producers declare this as a plain external and ensure it is
//     linked whenever the symbol is referenced. The adaptive runtime
//     (libtopo-adaptive) references topo_pass_event_emit
//     unconditionally from its monitor, so injectAutoLinkLibs() treats
//     topo-pass-event as a transitive dependency of topo-adaptive:
//     whenever topo-adaptive is linked (whether auto-injected for
//     [adaptive] or named explicitly in [build].link_libs), -ltopo-
//     pass-event is added after it (see
//     topo-core/include/topo/Build/AutoLink.h). Future producers
//     (LifetimeArena / TopoParallel) that reference this symbol from
//     their own runtime libs must add the equivalent transitive entry.

/// ABI version — bump when any signature in this header changes (add/remove
/// parameter, change parameter or return type, change calling convention,
/// or change the NDJSON record schema in a non-backward-compatible way).
/// Adding a NEW entry point without touching existing ones, OR adding an
/// OPTIONAL JSON field that older consumers can ignore, is backward
/// compatible and does NOT require a bump.
///
/// Consumers obtain the runtime's actual version via topo_pass_event_version()
/// at process start and compare against TOPO_PASS_EVENT_ABI_VERSION; mismatch
/// is a hard error. See topo-llvm/runtime/ABI-COMPAT.md for the compatibility
/// matrix and bump history.
///
/// v1: initial pinned baseline (topo_pass_event_emit + topo_pass_event_emit_sized;
///     NDJSON record schema as documented above).
#define TOPO_PASS_EVENT_ABI_VERSION 1

/// Report the runtime's compiled-in TOPO_PASS_EVENT_ABI_VERSION. Consumers
/// (typically a one-time ctor injected by AdaptiveDispatchPass /
/// LifetimeArenaPass) call this and abort on mismatch with their own
/// TOPO_PASS_EVENT_ABI_VERSION.
uint32_t topo_pass_event_version(void);

/// Emit one pass-event NDJSON record on stdout.
///
/// @param pass     Emitting Pass name (e.g. "AdaptiveDispatchPass").
/// @param from     Variant in effect before the switch (e.g. "aot").
/// @param to       Variant in effect after the switch (e.g. "jit").
/// @param subject  Optional pipeline/scope name; null or "" -> omitted.
void topo_pass_event_emit(const char* pass,
                          const char* from,
                          const char* to,
                          const char* subject);

/// Emit one pass-event NDJSON record carrying a numeric magnitude.
///
/// Identical to topo_pass_event_emit() but additionally writes a
/// `"bytes":<int64>` key. Kept as a SEPARATE entry point (rather than
/// adding a parameter to topo_pass_event_emit) so the existing
/// AdaptiveDispatch producers — which call the 4-arg form — produce
/// byte-for-byte identical output and no caller needs to be touched.
///
/// @param pass     Emitting Pass name (e.g. "LifetimeArenaPass").
/// @param from     State before the moment (e.g. "heap").
/// @param to       State after the moment (e.g. "arena").
/// @param subject  Optional lifetime scope name; null or "" -> omitted.
/// @param bytes    Numeric magnitude (e.g. arena capacity / bytes used).
void topo_pass_event_emit_sized(const char* pass,
                                const char* from,
                                const char* to,
                                const char* subject,
                                int64_t bytes);

#ifdef __cplusplus
}
#endif

#endif // TOPO_RT_PASS_EVENT_RT_H
