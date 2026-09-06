#!/usr/bin/env python3
"""Cross-workload confirmation for CALM's sample-normalized budget bundles."""

from __future__ import annotations

import argparse
import csv
import math
from datetime import datetime
from pathlib import Path

from calm_generalization_tune import (
    FIXED_PARAMS,
    RESULT_ROOT,
    VALIDATION,
    Runner,
    normalized_budget,
    number,
)


BUDGET_BUNDLES = {
    "user_quarter_half_2": normalized_budget(0.25, 0.5, 2.0, 0.25),
    "fixed_ratio_eighth_half_2": normalized_budget(0.125, 0.5, 2.0, 0.25),
    "initial_one": normalized_budget(0.25, 1.0, 2.0, 0.25),
    "large_initial_selected": normalized_budget(0.25, 3.0, 3.0, 0.25),
    "large_min": normalized_budget(0.5, 1.0, 2.0, 0.25),
}


def load_fixed_baselines(path: Path) -> dict[str, dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return {
            row["workload"]: row
            for row in csv.DictReader(stream)
            if row["stage"] == "validation" and row["candidate"] == "fixed_tuned"
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-runs", type=Path, required=True)
    parser.add_argument("--timeout-s", type=float, default=120.0)
    args = parser.parse_args()

    baselines = load_fixed_baselines(args.baseline_runs)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    root = RESULT_ROOT / f"CALM3_generalization_budget_confirm_{stamp}"
    root.mkdir(parents=True, exist_ok=False)
    runner = Runner(root, args.timeout_s)

    # Five lambda<mu workloads plus the lambda>=mu workload where the fixed
    # controller timed out.  The other two overload cases already completed
    # with the fixed controller and are retained for the final rate validation.
    workloads = tuple(VALIDATION[:5]) + (VALIDATION[-1],)
    for workload in workloads:
        for name, budget in BUDGET_BUNDLES.items():
            runner.run(
                stage="budget_confirmation",
                candidate=name,
                workload=workload,
                parameters={**FIXED_PARAMS, **budget},
            )

    ranking: list[dict[str, object]] = []
    for name in BUDGET_BUNDLES:
        rows = [row for row in runner.rows if row["candidate"] == name]
        ratios: list[float] = []
        complete = 0
        for row in rows:
            baseline = baselines[row["workload"]]
            baseline_p95 = max(1.0, number(baseline, "sub_delay_p95_ms"))
            ratios.append(number(row, "sub_delay_p95_ms") / baseline_p95)
            complete += int(
                number(row, "received_count") >= number(row, "requested_count")
                and not number(row, "timed_out"))
        geometric_mean = math.exp(sum(math.log(max(1e-9, ratio)) for ratio in ratios) / len(ratios))
        ranking.append({
            "candidate": name,
            "complete_runs": complete,
            "runs": len(rows),
            "p95_ratio_geomean": geometric_mean,
            "p95_ratio_worst": max(ratios),
            "p95_improved_runs": sum(ratio < 1.0 for ratio in ratios),
        })
    ranking.sort(key=lambda row: (
        -int(row["complete_runs"]),
        float(row["p95_ratio_geomean"]),
        float(row["p95_ratio_worst"]),
    ))
    with (root / "budget_bundle_ranking.csv").open(
            "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(ranking[0]))
        writer.writeheader()
        writer.writerows(ranking)
    print(f"[CONFIRM] best={ranking[0]['candidate']}")
    print(f"[CONFIRM] results={root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
