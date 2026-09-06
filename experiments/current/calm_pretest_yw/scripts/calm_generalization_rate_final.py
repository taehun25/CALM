#!/usr/bin/env python3
"""Complete the cross-workload validation for the leading rate bundles."""

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


def main() -> int:
    root = RESULT_ROOT / (
        "CALM3_generalization_rate_final_" +
        datetime.now().strftime("%Y%m%d_%H%M%S"))
    root.mkdir(parents=True, exist_ok=False)
    runner = Runner(root, 120.0)
    budget = normalized_budget(0.25, 1.0, 2.0, 0.25)
    candidates = {
        "fixed_64_192": {},
        "offered_balanced": normalized_rate(0.5, 1.5, 2.5, 0.125),
        "offered_fast": normalized_rate(0.75, 2.0, 3.0, 0.125),
    }
    workloads = (VALIDATION[1], VALIDATION[3], VALIDATION[5], VALIDATION[6])
    for workload in workloads:
        for name, rate in candidates.items():
            runner.run(
                stage="rate_final",
                candidate=name,
                workload=workload,
                parameters={**FIXED_PARAMS, **budget, **rate},
            )
    print(f"[RATE-FINAL] results={root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
