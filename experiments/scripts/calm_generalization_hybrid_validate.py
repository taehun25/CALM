#!/usr/bin/env python3
"""Validate floor-anchored sample scaling over all CALM workloads."""

from __future__ import annotations

from datetime import datetime

from calm_generalization_tune import (
    FIXED_PARAMS,
    RESULT_ROOT,
    VALIDATION,
    Runner,
    normalized_budget,
)


def main() -> int:
    root = RESULT_ROOT / (
        "CALM3_generalization_hybrid_" +
        datetime.now().strftime("%Y%m%d_%H%M%S"))
    root.mkdir(parents=True, exist_ok=False)
    runner = Runner(root, 120.0)
    parameters = {
        **FIXED_PARAMS,
        **normalized_budget(0.25, 1.0, 2.0, 0.25),
        "calm-budget-scaling-mode": "floor",
    }
    for workload in VALIDATION:
        runner.run(
            stage="hybrid_validation",
            candidate="floor_anchored",
            workload=workload,
            parameters=parameters,
        )
    print(f"[HYBRID] results={root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
