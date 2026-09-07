#!/usr/bin/env python3
"""Cross-workload screen for Writer-observed CALM release-rate scaling."""

from __future__ import annotations

from datetime import datetime

from calm_generalization_tune import (
    FIXED_PARAMS,
    RESULT_ROOT,
    VALIDATION,
    Runner,
    normalized_budget,
    normalized_rate,
)


BUDGET = normalized_budget(0.25, 1.0, 2.0, 0.25)
RATE_BUNDLES = {
    "fixed_64_192": {},
    "offered_low": normalized_rate(0.5, 1.0, 2.0, 0.0625),
    "offered_balanced": normalized_rate(0.5, 1.5, 2.5, 0.125),
    "offered_fast": normalized_rate(0.75, 2.0, 3.0, 0.125),
    "offered_aggressive": normalized_rate(1.0, 2.0, 4.0, 0.25),
    "ack_goodput_2x": {
        **normalized_rate(0.5, 1.5, 3.0, 0.125),
        "calm-release-rate-mode": "ack_goodput",
        "calm-ack-goodput-headroom": 2.0,
    },
}


def main() -> int:
    root = RESULT_ROOT / (
        "CALM3_generalization_rate_confirm_" +
        datetime.now().strftime("%Y%m%d_%H%M%S"))
    root.mkdir(parents=True, exist_ok=False)
    runner = Runner(root, 120.0)
    workloads = (VALIDATION[0], VALIDATION[2], VALIDATION[4], VALIDATION[-1])
    for workload in workloads:
        for name, rate in RATE_BUNDLES.items():
            runner.run(
                stage="rate_confirmation",
                candidate=name,
                workload=workload,
                parameters={**FIXED_PARAMS, **BUDGET, **rate},
            )
    print(f"[RATE-CONFIRM] results={root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
