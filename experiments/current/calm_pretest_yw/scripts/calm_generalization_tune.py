#!/usr/bin/env python3
"""Tune sample/rate-normalized CALM parameters on loopback + netem.

The search is deliberately staged.  Exhaustively multiplying every candidate
would mix parameter effects and produce hundreds of low-value runs.  Each axis
is screened on the same transient-loss workload, then the selected normalized
configuration is compared with the fixed 1 MiB-tuned configuration over
multiple offered loads.
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any


SCRIPT = Path(__file__).with_name("ddsopt_loopback_automation.py")
RESULT_ROOT = Path("/home/csi/ros2_ws/results/test_yw_2")


@dataclass(frozen=True)
class Workload:
    name: str
    payload_kb: int
    hz: int
    scenario: str
    count: int
    loss: float = 0.0
    disturbance_s: float = 5.0
    bg_payload_kb: int = 512
    bg_hz: int = 20
    bg_flows: int = 2

    @property
    def offered_mbps(self) -> float:
        return self.payload_kb * 1024 * 8 * self.hz / 1_000_000


PRIMARY = Workload("primary_a20", 1024, 10, "case_a", 240, loss=20.0)
VALIDATION = (
    Workload("lt_p256_h20_a10", 256, 20, "case_a", 300, loss=10.0),
    Workload("lt_p512_h20_a20", 512, 20, "case_a", 300, loss=20.0),
    Workload("lt_p1024_h10_d5", 1024, 10, "case_d", 300),
    Workload("lt_p512_h10_l2", 512, 10, "case_l", 300),
    Workload("lt_p512_h30_a10", 512, 30, "case_a", 300, loss=10.0),
    Workload("ge_p1024_h25_a10", 1024, 25, "case_a", 240, loss=10.0),
    Workload("ge_p1024_h30_a10", 1024, 30, "case_a", 240, loss=10.0),
    Workload("ge_p2048_h15_d5", 2048, 15, "case_d", 240),
)


FIXED_PARAMS: dict[str, Any] = {
    "calm-min-budget-bytes": 128 * 1024,
    "calm-initial-budget-bytes": 512 * 1024,
    "calm-max-budget-bytes": 2 * 1024 * 1024,
    "calm-ai-bytes": 256 * 1024,
    "calm-min-release-rate-mbps": 64,
    "calm-initial-release-rate-mbps": 192,
    "calm-max-release-rate-mbps": 192,
    "calm-rate-ai-mbps": 16,
    # Preserve the historical fixed baseline inside tuning scripts. Candidate
    # stages explicitly override these values when testing sample scaling.
    "calm-min-budget-sample-multiplier": 0.0,
    "calm-initial-budget-sample-multiplier": 0.0,
    "calm-max-budget-sample-multiplier": 0.0,
    "calm-ai-b-sample-multiplier": 0.0,
    "calm-budget-scaling-mode": "replace",
}


def read_one(path: Path) -> dict[str, str]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise RuntimeError(f"expected one summary row in {path}, got {len(rows)}")
    return rows[0]


def number(row: dict[str, str], name: str) -> float:
    try:
        return float(row.get(name, "") or 0.0)
    except ValueError:
        return 0.0


def result_path(output: str) -> Path:
    marker = "[DDSOPT-LO] results: "
    for line in reversed(output.splitlines()):
        if line.startswith(marker):
            return Path(line[len(marker):].strip())
    raise RuntimeError("automation did not report its result directory")


def parameter_args(parameters: dict[str, Any]) -> list[str]:
    arguments: list[str] = []
    for name, value in parameters.items():
        arguments.extend((f"--{name}", str(value)))
    return arguments


def normalized_budget(bmin: float, initial: float, bmax: float, ai: float) -> dict[str, Any]:
    return {
        "calm-min-budget-sample-multiplier": bmin,
        "calm-initial-budget-sample-multiplier": initial,
        "calm-max-budget-sample-multiplier": bmax,
        "calm-ai-b-sample-multiplier": ai,
    }


def normalized_rate(vmin: float, initial: float, vmax: float, ai: float) -> dict[str, Any]:
    return {
        "calm-min-rate-offered-multiplier": vmin,
        "calm-initial-rate-offered-multiplier": initial,
        "calm-max-rate-offered-multiplier": vmax,
        "calm-ai-v-offered-multiplier": ai,
    }


@dataclass
class Runner:
    root: Path
    timeout_s: float
    next_run: int = 1
    rows: list[dict[str, Any]] = field(default_factory=list)

    def persist(self) -> None:
        if not self.rows:
            return
        path = self.root / "generalization_runs.csv"
        fields: list[str] = []
        for row in self.rows:
            for name in row:
                if name not in fields:
                    fields.append(name)
        temporary = path.with_suffix(".tmp")
        with temporary.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(self.rows)
        temporary.replace(path)

    def run(
            self,
            *,
            stage: str,
            candidate: str,
            workload: Workload,
            control: str = "calm",
            parameters: dict[str, Any] | None = None,
            repeat: int = 1) -> dict[str, Any]:
        run_number = self.next_run
        self.next_run += 1
        label = f"generalize_{run_number:03d}_{stage}_{candidate}_r{repeat}"
        trigger_after = max(10, min(40, workload.count // 5))
        command = [
            sys.executable, str(SCRIPT),
            "--label", label,
            "--modes", "opt12",
            "--controls", control,
            "--scenarios", workload.scenario,
            "--payload-kb", str(workload.payload_kb),
            "--hz", str(workload.hz),
            "--count", str(workload.count),
            "--max-samples", str(workload.count),
            "--trigger-after", str(trigger_after),
            "--case-duration-s", str(workload.disturbance_s),
            "--case-a-duration-s", str(workload.disturbance_s),
            "--case-a-loss-percent", str(workload.loss),
            "--bg-payload-kb", str(workload.bg_payload_kb),
            "--bg-hz", str(workload.bg_hz),
            "--bg-flows", str(workload.bg_flows),
            "--timeout-s", str(self.timeout_s),
            "--domain-base", str(170 + run_number % 50),
            "--no-storm-log",
            "--calm-rho-sample-multiplier", "2",
            "--calm-retry-signal", "rho_progress",
            "--calm-growth-rounds", "2",
            "--calm-ack-stall-ms", "500",
        ]
        if parameters:
            command.extend(parameter_args(parameters))
        print(
            f"[GENERALIZE] {run_number} {stage}/{candidate} r{repeat} "
            f"{workload.name} lambda={workload.offered_mbps:.2f}Mbps",
            flush=True,
        )
        completed = subprocess.run(
            command, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False)
        print(completed.stdout, end="", flush=True)
        if completed.returncode != 0:
            raise RuntimeError(
                f"run failed ({completed.returncode}): {' '.join(command)}")
        output_dir = result_path(completed.stdout)
        summary = read_one(output_dir / "summary.csv")
        row: dict[str, Any] = {
            "stage": stage,
            "candidate": candidate,
            "repeat": repeat,
            "workload": workload.name,
            "lambda_mbps": workload.offered_mbps,
            "lambda_class": "lt_mu" if workload.offered_mbps < 180 else "ge_mu",
            "control": control,
            **(parameters or {}),
            **summary,
            "result_dir": str(output_dir),
        }
        self.rows.append(row)
        self.persist()
        return row


def candidate_score(row: dict[str, Any], requested: int) -> float:
    received = number(row, "received_count")
    completion_penalty = max(0.0, requested - received) * 1_000_000.0
    timeout_penalty = 1_000_000_000.0 if number(row, "timed_out") else 0.0
    target_hz = max(1.0, number(row, "hz"))
    hz_shortfall = max(0.0, target_hz - number(row, "sub_actual_hz")) / target_hz
    return (
        completion_penalty + timeout_penalty +
        number(row, "sub_delay_p95_ms") +
        0.15 * number(row, "sub_delay_max_ms") +
        0.10 * number(row, "sub_delay_std_ms") +
        1000.0 * hz_shortfall
    )


def best(rows: list[dict[str, Any]], stage: str, requested: int) -> dict[str, Any]:
    candidates = [row for row in rows if row["stage"] == stage]
    if not candidates:
        raise RuntimeError(f"stage {stage} produced no rows")
    return min(candidates, key=lambda row: candidate_score(row, requested))


def median_metrics(rows: list[dict[str, Any]]) -> dict[str, float]:
    keys = (
        "received_count", "sub_delay_mean_ms", "sub_delay_p95_ms",
        "sub_delay_max_ms", "sub_delay_std_ms", "sub_actual_hz",
        "rho_max_bytes", "elapsed_s", "calm_budget_min_bytes",
        "calm_budget_max_bytes", "calm_release_rate_min_mbps",
        "calm_release_rate_max_mbps",
    )
    return {
        key: statistics.median(number(row, key) for row in rows)
        for key in keys
    }


def write_ranking(runner: Runner) -> None:
    path = runner.root / "stage_ranking.csv"
    output: list[dict[str, Any]] = []
    for stage in sorted({str(row["stage"]) for row in runner.rows}):
        grouped: dict[str, list[dict[str, Any]]] = {}
        for row in runner.rows:
            if row["stage"] == stage:
                grouped.setdefault(str(row["candidate"]), []).append(row)
        for candidate, rows in grouped.items():
            metrics = median_metrics(rows)
            requested = int(max(number(row, "requested_count") for row in rows))
            output.append({
                "stage": stage,
                "candidate": candidate,
                "runs": len(rows),
                "complete_runs": sum(
                    number(row, "received_count") >= number(row, "requested_count")
                    and not number(row, "timed_out") for row in rows),
                **metrics,
                "score": statistics.median(
                    candidate_score(row, requested) for row in rows),
            })
    output.sort(key=lambda row: (row["stage"], float(row["score"])))
    if output:
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(output[0]))
            writer.writeheader()
            writer.writerows(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--result-root", type=Path, default=RESULT_ROOT)
    parser.add_argument("--timeout-s", type=float, default=120.0)
    parser.add_argument("--skip-validation", action="store_true")
    args = parser.parse_args()

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    root = args.result_root / f"CALM3_generalization_tuning_{stamp}"
    root.mkdir(parents=True, exist_ok=False)
    runner = Runner(root, args.timeout_s)

    # F is deliberately compared as an optional activation gate before tuning.
    for repeat in (1, 2):
        runner.run(
            stage="f_gate", candidate="rho_progress", workload=PRIMARY,
            parameters=FIXED_PARAMS, repeat=repeat)
        f1 = dict(FIXED_PARAMS)
        f1.update({"calm-retry-signal": "f", "calm-failure-count": 1})
        runner.run(
            stage="f_gate", candidate="and_f_ge_1", workload=PRIMARY,
            parameters=f1, repeat=repeat)

    # Budget axes.  Very small B_min is used while screening B_initial so every
    # requested initial candidate is mathematically valid.
    for value in (3, 2, 1, 0.5, 0.25, 0.125, 0.0625):
        params = normalized_budget(0.0625, value, 4, 0.25)
        runner.run(
            stage="budget_initial", candidate=f"sbar_x{value:g}",
            workload=PRIMARY, parameters=params)
    selected_initial = number(
        best(runner.rows, "budget_initial", PRIMARY.count),
        "calm-initial-budget-sample-multiplier")

    min_candidates = (1, 0.5, 0.25, 0.125, 0.0625)
    valid_min = [value for value in min_candidates if value <= selected_initial]
    for value in valid_min:
        params = normalized_budget(value, selected_initial, 4, 0.25)
        runner.run(
            stage="budget_min", candidate=f"sbar_x{value:g}",
            workload=PRIMARY, parameters=params)
    selected_min = number(
        best(runner.rows, "budget_min", PRIMARY.count),
        "calm-min-budget-sample-multiplier")

    max_candidates = (4, 3, 2, 1, 0.5)
    valid_max = [value for value in max_candidates if value >= selected_initial]
    for value in valid_max:
        params = normalized_budget(selected_min, selected_initial, value, 0.25)
        runner.run(
            stage="budget_max", candidate=f"sbar_x{value:g}",
            workload=PRIMARY, parameters=params)
    selected_max = number(
        best(runner.rows, "budget_max", PRIMARY.count),
        "calm-max-budget-sample-multiplier")

    for value in (3, 2, 1, 0.5, 0.25, 0.125, 0.0625):
        params = normalized_budget(selected_min, selected_initial, selected_max, value)
        runner.run(
            stage="budget_ai", candidate=f"sbar_x{value:g}",
            workload=PRIMARY, parameters=params)
    selected_ai = number(
        best(runner.rows, "budget_ai", PRIMARY.count),
        "calm-ai-b-sample-multiplier")
    budget_params = normalized_budget(
        selected_min, selected_initial, selected_max, selected_ai)

    # Release-rate axes use the Writer-observed offered-rate EWMA as reference.
    for value in (0.5, 1, 1.5, 2, 2.5, 3):
        params = {**budget_params, **normalized_rate(0.125, value, 4, 0.125)}
        runner.run(
            stage="rate_initial", candidate=f"lambda_x{value:g}",
            workload=PRIMARY, parameters=params)
    selected_rate_initial = number(
        best(runner.rows, "rate_initial", PRIMARY.count),
        "calm-initial-rate-offered-multiplier")

    for value in (1, 0.75, 0.5, 0.25, 0.125):
        if value > selected_rate_initial:
            continue
        params = {
            **budget_params,
            **normalized_rate(value, selected_rate_initial, 4, 0.125),
        }
        runner.run(
            stage="rate_min", candidate=f"lambda_x{value:g}",
            workload=PRIMARY, parameters=params)
    selected_rate_min = number(
        best(runner.rows, "rate_min", PRIMARY.count),
        "calm-min-rate-offered-multiplier")

    for value in (1, 1.5, 2, 3, 4):
        if value < selected_rate_initial:
            continue
        params = {
            **budget_params,
            **normalized_rate(selected_rate_min, selected_rate_initial, value, 0.125),
        }
        runner.run(
            stage="rate_max", candidate=f"lambda_x{value:g}",
            workload=PRIMARY, parameters=params)
    selected_rate_max = number(
        best(runner.rows, "rate_max", PRIMARY.count),
        "calm-max-rate-offered-multiplier")

    for value in (0.5, 0.25, 0.125, 0.0625, 0.03125):
        params = {
            **budget_params,
            **normalized_rate(
                selected_rate_min, selected_rate_initial,
                selected_rate_max, value),
        }
        runner.run(
            stage="rate_ai", candidate=f"lambda_x{value:g}",
            workload=PRIMARY, parameters=params)
    selected_rate_ai = number(
        best(runner.rows, "rate_ai", PRIMARY.count),
        "calm-ai-v-offered-multiplier")

    final_params = {
        **budget_params,
        **normalized_rate(
            selected_rate_min, selected_rate_initial,
            selected_rate_max, selected_rate_ai),
    }
    (root / "selected_parameters.json").write_text(json.dumps({
        "budget": budget_params,
        "rate": final_params,
        "fixed_reference": FIXED_PARAMS,
        "link_rate_mbps": 180,
    }, indent=2) + "\n", encoding="utf-8")

    if not args.skip_validation:
        for workload in VALIDATION:
            runner.run(
                stage="validation", candidate="fixed_tuned",
                workload=workload, parameters=FIXED_PARAMS)
            runner.run(
                stage="validation", candidate="normalized",
                workload=workload, parameters=final_params)

    write_ranking(runner)
    print(f"[GENERALIZE] selected={json.dumps(final_params, sort_keys=True)}")
    print(f"[GENERALIZE] results={root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
