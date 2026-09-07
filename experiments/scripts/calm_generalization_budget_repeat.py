#!/usr/bin/env python3
"""Repeat the leading normalized-budget candidates on sensitive workloads."""

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
        "CALM3_generalization_budget_repeat_" +
        datetime.now().strftime("%Y%m%d_%H%M%S"))
    root.mkdir(parents=True, exist_ok=False)
    runner = Runner(root, 120.0)
    candidates = {
        "fixed_ratio_half": normalized_budget(0.125, 0.5, 2.0, 0.25),
        "initial_one": normalized_budget(0.25, 1.0, 2.0, 0.25),
        "initial_three": normalized_budget(0.25, 3.0, 3.0, 0.25),
    }
    workloads = (VALIDATION[1], VALIDATION[2], VALIDATION[4])
    for repeat in (1, 2):
        for workload in workloads:
            for name, budget in candidates.items():
                runner.run(
                    stage="budget_repeat",
                    candidate=name,
                    workload=workload,
                    parameters={**FIXED_PARAMS, **budget},
                    repeat=repeat,
                )
    print(f"[BUDGET-REPEAT] results={root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
