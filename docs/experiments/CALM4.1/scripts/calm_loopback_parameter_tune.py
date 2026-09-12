#!/usr/bin/env python3
"""Screen CALM repair-budget parameters against a fixed loopback baseline."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from datetime import datetime
from pathlib import Path


MIB = 1024 * 1024
SCRIPT = Path(__file__).with_name("ddsopt_loopback_automation.py")
RESULT_ROOT = Path("/home/csi/ros2_ws/results/test_yw_2")


CONFIGS = [
    # name, max/initial B, min B, gamma, AI, failed rounds, horizon, rho_soft, pacing floor
    ("b4_min05_g075_f1_h20", 4*MIB, 512*1024, .75, 256*1024, 1, 20, 2*MIB, 5),
    ("b8_min1_g075_f1_h20", 8*MIB, 1*MIB, .75, 512*1024, 1, 20, 2*MIB, 5),
    ("b16_min2_g075_f1_h20", 16*MIB, 2*MIB, .75, 1*MIB, 1, 20, 2*MIB, 5),
    ("b4_min1_g0875_f2_h20", 4*MIB, 1*MIB, .875, 512*1024, 2, 20, 2*MIB, 5),
    ("b8_min2_g0875_f2_h20", 8*MIB, 2*MIB, .875, 1*MIB, 2, 20, 2*MIB, 5),
    ("b16_min4_g0875_f2_h20", 16*MIB, 4*MIB, .875, 2*MIB, 2, 20, 2*MIB, 5),
    ("b4_min1_g09_f3_h20", 4*MIB, 1*MIB, .90, 512*1024, 3, 20, 2*MIB, 5),
    ("b8_min2_g09_f3_h20", 8*MIB, 2*MIB, .90, 1*MIB, 3, 20, 2*MIB, 5),
    ("b16_min4_g09_f3_h20", 16*MIB, 4*MIB, .90, 2*MIB, 3, 20, 2*MIB, 5),
    ("b4_min1_g0875_f2_h10", 4*MIB, 1*MIB, .875, 512*1024, 2, 10, 2*MIB, 5),
    ("b8_min2_g0875_f2_h10", 8*MIB, 2*MIB, .875, 1*MIB, 2, 10, 2*MIB, 5),
    ("b8_min2_g0875_f2_h30", 8*MIB, 2*MIB, .875, 1*MIB, 2, 30, 2*MIB, 5),
    ("b8_min2_g0875_f2_h50", 8*MIB, 2*MIB, .875, 1*MIB, 2, 50, 2*MIB, 5),
    ("b8_min2_g09_f3_h20_r4", 8*MIB, 2*MIB, .90, 1*MIB, 3, 20, 4*MIB, 5),
    ("b8_min2_g09_f3_h20_r8", 8*MIB, 2*MIB, .90, 1*MIB, 3, 20, 8*MIB, 5),
    ("b4_min1_g095_f4_h20", 4*MIB, 1*MIB, .95, 512*1024, 4, 20, 2*MIB, 5),
    ("b8_min2_g095_f4_h20", 8*MIB, 2*MIB, .95, 1*MIB, 4, 20, 2*MIB, 5),
    ("b16_min4_g095_f4_h20", 16*MIB, 4*MIB, .95, 2*MIB, 4, 20, 2*MIB, 5),
    ("b8_min2_g09_f3_h20_p1", 8*MIB, 2*MIB, .90, 1*MIB, 3, 20, 2*MIB, 1),
    ("b8_min2_g09_f3_h20_p10", 8*MIB, 2*MIB, .90, 1*MIB, 3, 20, 2*MIB, 10),
]


def read_one(path: Path) -> dict[str, str]:
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) != 1:
        raise RuntimeError(f"expected one summary row in {path}, got {len(rows)}")
    return rows[0]


def number(row: dict[str, str], name: str) -> float:
    value = row.get(name, "")
    return float(value) if value else 0.0


def find_result_path(output: str) -> Path:
    marker = "[DDSOPT-LO] results: "
    for line in reversed(output.splitlines()):
        if line.startswith(marker):
            return Path(line[len(marker):].strip())
    raise RuntimeError("automation output did not report a result directory")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-summary", type=Path, required=True)
    parser.add_argument("--max-runs", type=int, default=20)
    parser.add_argument("--loss", type=float, default=40.0)
    parser.add_argument("--count", type=int, default=400)
    parser.add_argument("--timeout-s", type=float, default=90.0)
    args = parser.parse_args()

    baseline = read_one(args.baseline_summary)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    tune_dir = RESULT_ROOT / f"CALM3_parameter_tune_p1024_h10_l{args.loss:g}_{stamp}"
    tune_dir.mkdir(parents=True, exist_ok=False)
    aggregate_path = tune_dir / "parameter_ranking.csv"
    manifest_path = tune_dir / "manifest.json"
    manifest_path.write_text(json.dumps({
        "baseline_summary": str(args.baseline_summary),
        "baseline": baseline,
        "loss_percent": args.loss,
        "count": args.count,
        "max_runs": min(args.max_runs, len(CONFIGS)),
    }, indent=2) + "\n")

    fields = [
        "name", "max_budget", "min_budget", "gamma", "ai", "failed_rounds",
        "horizon_ms", "soft_rho", "pacing_ms", "received_count", "timed_out",
        "elapsed_s", "sub_delay_mean_ms", "sub_delay_p95_ms", "rho_max_bytes",
        "sample_retransmit_count_p95", "calm_active_rows", "p95_ratio",
        "rho_ratio", "score", "result_dir",
    ]
    with aggregate_path.open("w", newline="") as handle:
        csv.DictWriter(handle, fieldnames=fields).writeheader()

    baseline_p95 = max(1.0, number(baseline, "sub_delay_p95_ms"))
    baseline_rho = max(1.0, number(baseline, "rho_max_bytes"))
    baseline_elapsed = max(1.0, number(baseline, "elapsed_s"))
    baseline_retry = max(1.0, number(baseline, "sample_retransmit_count_p95"))

    for index, config in enumerate(CONFIGS[:args.max_runs], start=1):
        name, max_b, min_b, gamma, ai, failed, horizon, soft_rho, pacing = config
        command = [
            sys.executable, str(SCRIPT),
            "--label", f"tune_{index:02d}_{name}",
            "--modes", "opt12", "--controls", "calm", "--scenarios", "case_a",
            "--case-a-losses", f"{args.loss:g}", "--payload-kb", "1024", "--hz", "10",
            "--count", str(args.count), "--max-samples", str(args.count),
            "--trigger-after", "40", "--timeout-s", str(args.timeout_s),
            "--calm-max-budget-bytes", str(max_b),
            "--calm-initial-budget-bytes", str(max_b),
            "--calm-min-budget-bytes", str(min_b),
            "--calm-gamma", str(gamma), "--calm-ai-bytes", str(ai),
            "--calm-failed-rounds-before-decrease", str(failed),
            "--calm-budget-horizon-ms", str(horizon),
            "--calm-rho-bytes", str(soft_rho),
            "--calm-retry-count", "2",
            "--calm-growth-rounds", "2", "--calm-pacing-ms", str(pacing),
        ]
        print(f"[TUNE] {index}/{min(args.max_runs, len(CONFIGS))} {name}", flush=True)
        completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, check=False)
        print(completed.stdout, end="", flush=True)
        result_dir = find_result_path(completed.stdout)
        row = read_one(result_dir / "summary.csv")

        received = int(number(row, "received_count"))
        p95_ratio = number(row, "sub_delay_p95_ms") / baseline_p95
        rho_ratio = number(row, "rho_max_bytes") / baseline_rho
        elapsed_ratio = number(row, "elapsed_s") / baseline_elapsed
        retry_ratio = number(row, "sample_retransmit_count_p95") / baseline_retry
        completion_penalty = 0.0 if received == args.count else 10.0 + (args.count - received) / args.count
        score = completion_penalty + p95_ratio + .25*rho_ratio + .10*elapsed_ratio + .05*retry_ratio
        result = {
            "name": name, "max_budget": max_b, "min_budget": min_b,
            "gamma": gamma, "ai": ai, "failed_rounds": failed,
            "horizon_ms": horizon, "soft_rho": soft_rho, "pacing_ms": pacing,
            "received_count": received, "timed_out": row["timed_out"],
            "elapsed_s": row["elapsed_s"], "sub_delay_mean_ms": row["sub_delay_mean_ms"],
            "sub_delay_p95_ms": row["sub_delay_p95_ms"], "rho_max_bytes": row["rho_max_bytes"],
            "sample_retransmit_count_p95": row["sample_retransmit_count_p95"],
            "calm_active_rows": row["calm_active_rows"], "p95_ratio": p95_ratio,
            "rho_ratio": rho_ratio, "score": score, "result_dir": str(result_dir),
        }
        with aggregate_path.open("a", newline="") as handle:
            csv.DictWriter(handle, fieldnames=fields).writerow(result)
        print(f"[TUNE] recv={received}/{args.count} p95_ratio={p95_ratio:.3f} "
              f"rho_ratio={rho_ratio:.3f} score={score:.3f}", flush=True)

    rows = []
    with aggregate_path.open(newline="") as handle:
        rows.extend(csv.DictReader(handle))
    rows.sort(key=lambda item: float(item["score"]))
    ranking_path = tune_dir / "parameter_ranking_sorted.csv"
    with ranking_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    print(f"[TUNE] ranking: {ranking_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
