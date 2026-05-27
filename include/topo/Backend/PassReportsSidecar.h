#ifndef TOPO_BACKEND_PASSREPORTSSIDECAR_H
#define TOPO_BACKEND_PASSREPORTSSIDECAR_H

// Writer for the per-Pass sidecar directory.
//
// Given a populated PassReports aggregate and the final binary's outputPath,
// creates `<outputPath>.topo-passes/` and writes one `<PassName>.json` per
// judging Pass. Pass that did not fire still gets a file with
// header.fired=false plus header.decision/reason explaining why; consumers
// (LSP / `topo debug` / users) can tell "Pass not run" from "Pass run but
// no candidates" without re-running the build.
//
// Each file is written atomically (write to .tmp + rename) so partial
// directories are never observable. Keys inside the JSON are sorted for
// golden-test stability.

#include "topo/Backend/PassReports.h"

#include <string>

namespace topo::backend {

// Returns true on full success; on partial / total failure prints to stderr
// and returns false (build continues — sidecar is non-load-bearing).
bool writePassReportsSidecar(const PassReports& reports,
                             const std::string& outputPath);

} // namespace topo::backend

#endif // TOPO_BACKEND_PASSREPORTSSIDECAR_H
