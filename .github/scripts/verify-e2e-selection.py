#!/usr/bin/env python3
"""Check actual CTest discovery and both selections before executing e2e."""

import json
import sys
from pathlib import Path


# Each sampling-dependent check keeps a corresponding daily correctness check.
# A new auto case needs an explicit coverage decision instead of disappearing
# behind a label exclusion.
AUTO_COUNTERPARTS = {
    "DataLayoutPass_DefaultMatchesVanilla": ["Equivalence.DataLayoutPass_ForcedMatchesVanilla"],
    "IndirectionPass_DefaultMatchesVanilla": ["Equivalence.IndirectionPass_ForcedMatchesVanilla"],
    "LoopParallelizePass_DefaultMatchesVanilla": ["Equivalence.LoopParallelizePass_ForcedMatchesVanilla"],
    "PrefetchPass_DefaultMatchesVanilla": ["Equivalence.PrefetchPass_ForcedMatchesVanilla"],
    "TopoParallelPass_BaseMatchesDefault": ["Equivalence.TopoParallelPass_BaseMatchesForced"],
    "TopoParallelPass_DefaultMatchesForced": ["Equivalence.TopoParallelPass_BaseMatchesForced"],
    "AdaptiveDispatchPass_BaseMatchesDefault": ["Equivalence.AdaptiveDispatchPass_BaseMatchesForced"],
    "AdaptiveDispatchPass_DefaultMatchesForced": ["Equivalence.AdaptiveDispatchPass_BaseMatchesForced"],
    "LifetimeArenaPass_BaseMatchesDefault": ["Equivalence.LifetimeArenaPass_BaseMatchesForced"],
    "LifetimeArenaPass_DefaultMatchesForced": ["Equivalence.LifetimeArenaPass_BaseMatchesForced"],
    "Ownership_DefaultMatchesVanilla": ["Functional.Ownership"],
}


def read_tests(path):
    tests = json.loads(Path(path).read_text(encoding="utf-8"))["tests"]
    by_name = {test["name"]: test for test in tests}
    if len(by_name) != len(tests):
        raise ValueError(f"duplicate CTest names in {path}")
    return by_name


def main():
    if len(sys.argv) != 4:
        raise ValueError("usage: verify-e2e-selection.py registered.json daily.json auto.json")
    registered, daily, auto = map(read_tests, sys.argv[1:])
    expected_auto = {"AutoSelection." + name for name in AUTO_COUNTERPARTS}
    discovered_auto = {name for name in registered if name.startswith("AutoSelection.")}
    if discovered_auto != expected_auto or set(auto) != expected_auto:
        raise ValueError(f"auto selection differs from coverage map: discovered={sorted(discovered_auto)}, selected={sorted(auto)}")
    if expected_auto.intersection(daily):
        raise ValueError("auto-selection sampling entered the daily gate")

    required = {
        "Functional.MixedLang", "Functional.RustBasic",
        "RustImplMethod.StructImplMethodsPassIRVerification",
        "Equivalence.Adaptive_FunctionalDispatch",
        "Equivalence.Observability_FunctionalEvents",
    }
    required.update(name for name in registered if name.startswith(("Functional.", "Equivalence.")))
    for counterparts in AUTO_COUNTERPARTS.values():
        required.update(counterparts)
    missing = required.difference(daily)
    if missing:
        raise ValueError(f"daily correctness coverage missing: {sorted(missing)}")
    for name, test in daily.items():
        labels = next((p["value"] for p in test.get("properties", []) if p["name"] == "LABELS"), [])
        if set(labels).intersection({"perf", "benchmark", "auto_selection"}):
            raise ValueError(f"sampling or timing test selected by daily gate: {name}")
    print(f"CTest selections verified: {len(daily)} daily, {len(auto)} explicit auto-selection checks")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
