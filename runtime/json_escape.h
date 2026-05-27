// Internal shared header for the topo-llvm runtime libs (NOT public).
//
// Provides a single inline JSON-string escaper used by every NDJSON
// emitter in this directory (topo_pass_event.cpp, topo_observe.cpp).
// Audit finding topo-llvm-pass-event-observe-json-unescaped: previously
// each emitter passed raw `%s` strings straight into fprintf, so any
// user-controlled name (pipeline / function / lifetime-scope) containing
// `"`, `\`, or a control character produced a malformed NDJSON line that
// broke topo-profile's record-aligned framing. Both emitters now route
// every string field through writeJsonString().
//
// Kept in topo-llvm/runtime/ rather than under include/ because the
// helper is private to these runtime libs (libtopo-pass-event,
// libtopo-observe). It deliberately does not depend on nlohmann::json
// (the runtime libs are C-ABI shims and must avoid heavy deps); a small
// in-tree implementation matches the existing escapeJson helper inside
// topo_jit_api.cpp.
//
// Output contract: writeJsonString writes the value INCLUDING the
// surrounding double quotes. Callers compose:
//
//     std::fprintf(f, "{\"name\":");
//     topo::rt::writeJsonString(f, name);
//     std::fprintf(f, ",\"ts_ns\":%lld}\n", ts);

#ifndef TOPO_RT_INTERNAL_JSON_ESCAPE_H
#define TOPO_RT_INTERNAL_JSON_ESCAPE_H

#include <cstdint>
#include <cstdio>

namespace topo::rt {

/// Write `s` to `f` as a JSON string literal (with surrounding quotes),
/// escaping per RFC 8259: `"` and `\` are backslash-escaped; the four
/// short forms `\b \f \n \r \t` are used where possible; every other
/// byte < 0x20 is emitted as `\u00XX`. Bytes >= 0x20 are passed through
/// verbatim (we do not re-encode UTF-8 — caller-provided bytes are
/// assumed already valid UTF-8 or the caller's responsibility).
///
/// A null pointer is written as the empty JSON string `""` to keep
/// parity with the previous `orEmpty(s)` shim that fed `%s`.
inline void writeJsonString(std::FILE* f, const char* s) {
    std::fputc('"', f);
    if (s) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
            unsigned char c = *p;
            switch (c) {
            case '"':  std::fputs("\\\"", f); break;
            case '\\': std::fputs("\\\\", f); break;
            case '\b': std::fputs("\\b",  f); break;
            case '\f': std::fputs("\\f",  f); break;
            case '\n': std::fputs("\\n",  f); break;
            case '\r': std::fputs("\\r",  f); break;
            case '\t': std::fputs("\\t",  f); break;
            default:
                if (c < 0x20) {
                    std::fprintf(f, "\\u%04x", static_cast<unsigned>(c));
                } else {
                    std::fputc(static_cast<int>(c), f);
                }
            }
        }
    }
    std::fputc('"', f);
}

} // namespace topo::rt

#endif // TOPO_RT_INTERNAL_JSON_ESCAPE_H
