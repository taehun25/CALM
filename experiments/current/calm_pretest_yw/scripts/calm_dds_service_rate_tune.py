#!/usr/bin/env python3
"""Tune and validate the DDS-only CALM release-rate estimator.

The search is intentionally small and staged.  It first screens dimensionless
service-rate multipliers on two application-limited loads and one overloaded
load.  It then compares OPT1+2 Default, the previous fixed-rate CALM, and the
selected DDS-only candidate over a broader workload set.
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


AUTOMATION = Path(__file__).with_name("ddsopt_loopback_automation.py")
DEFAULT_RESULT_ROOT = Path("/home/csi/ros2_ws/results/test_yw_2")
LINK_RATE_MBPS = 180.0


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

    @property
    def load_class(self) -> str:
        return "lt_mu" if self.offered_mbps < LINK_RATE_MBPS else "ge_mu"


SCREEN_WORKLOADS = (
    Workload("lt_p512_h20_a20", 512, 20, "case_a", 160, loss=20.0),
    Workload("lt_p1024_h10_a20", 1024, 10, "case_a", 160, loss=20.0),
    Workload("ge_p1024_h25_a10", 1024, 25, "case_a", 140, loss=10.0),
)

VALIDATION_WORKLOADS = (
    Workload("lt_p256_h20_a10", 256, 20, "case_a", 300, loss=10.0),
    Workload("lt_p512_h20_a20", 512, 20, "case_a", 300, loss=20.0),
    Workload("lt_p1024_h10_d5", 1024, 10, "case_d", 300),
    Workload("lt_p512_h10_l2", 512, 10, "case_l", 300),
    Workload("lt_p512_h30_a10", 512, 30, "case_a", 300, loss=10.0),
    Workload("ge_p1024_h25_a10", 1024, 25, "case_a", 240, loss=10.0),
    Workload("ge_p1024_h30_a10", 1024, 30, "case_a", 240, loss=10.0),
    Workload("ge_p2048_h15_d5", 2048, 15, "case_d", 240),
)


def service_parameters(
        minimum: float,
        initial: float,
        maximum: float,
        ai: float,
        alpha_up: float,
        alpha_down: float,
        failure_gamma: float,
        ack_cap: float = 1.05) -> dict[str, Any]:
    return {
        "calm-release-rate-mode": "dds_service",
        "calm-service-rate-min-multiplier": minimum,
        "calm-service-rate-initial-multiplier": initial,
        "calm-service-rate-max-multiplier": maximum,
        "calm-service-rate-ai-multiplier": ai,
        "calm-service-rate-alpha-up": alpha_up,
        "calm-service-rate-alpha-down": alpha_down,
        "calm-service-rate-failure-gamma": failure_gamma,
        "calm-service-rate-ack-cap-multiplier": ack_cap,
    }


CANDIDATES: dict[str, dict[str, Any]] = {
    "safe_window": service_parameters(0.25, 1.00, 2.0, 0.25, 0.25, 0.10, 0.98),
    "balanced_window": service_parameters(0.50, 1.50, 2.0, 0.25, 0.25, 0.10, 0.98),
    "brisk_window": service_parameters(0.50, 1.75, 2.5, 0.25, 0.25, 0.10, 0.98),
    "adaptive_window": service_parameters(0.25, 1.25, 3.0, 0.50, 0.40, 0.05, 0.97),
}


def read_one(path: Path) -> dict[str, str]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise RuntimeError(f"expected one row in {path}, got {len(rows)}")
    return rows[0]


def number(row: dict[str, Any], name: str) -> float:
    try:
        return float(row.get(name, "") or 0.0)
    except (TypeError, ValueError):
        return 0.0


def find_result(output: str) -> Path:
    marker = "[DDSOPT-LO] results: "
    for line in reversed(output.splitlines()):
        if line.startswith(marker):
            return Path(line[len(marker):].strip())
    raise RuntimeError("automation did not report its result directory")


def parameter_args(parameters: dict[str, Any]) -> list[str]:
    result: list[str] = []
    for name, value in parameters.items():
        result.extend((f"--{name}", str(value)))
    return result


def score(row: dict[str, Any]) -> float:
    requested = number(row, "requested_count")
    received = number(row, "received_count")
    missing = max(0.0, requested - received)
    target_hz = max(1.0, number(row, "hz"))
    hz_shortfall = max(0.0, target_hz - number(row, "sub_actual_hz")) / target_hz
    return (
        missing * 1_000_000.0 +
        (1_000_000_000.0 if number(row, "timed_out") else 0.0) +
        number(row, "sub_delay_p95_ms") +
        0.15 * number(row, "sub_delay_max_ms") +
        0.10 * number(row, "sub_delay_std_ms") +
        1000.0 * hz_shortfall
    )


@dataclass
class Runner:
    root: Path
    timeout_s: float
    dds: str = "fastdds"
    qos_depth: int = 1
    storm_log: bool = False
    next_run: int = 1
    rows: list[dict[str, Any]] = field(default_factory=list)

    def persist(self) -> None:
        if not self.rows:
            return
        fields: list[str] = []
        for row in self.rows:
            for name in row:
                if name not in fields:
                    fields.append(name)
        destination = self.root / "service_rate_runs.csv"
        temporary = destination.with_suffix(".tmp")
        with temporary.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(self.rows)
        temporary.replace(destination)

    def run(
            self,
            stage: str,
            candidate: str,
            workload: Workload,
            control: str,
            parameters: dict[str, Any] | None = None) -> dict[str, Any]:
        run_number = self.next_run
        self.next_run += 1
        trigger = max(10, min(40, workload.count // 5))
        label = f"dds_service_{run_number:03d}_{stage}_{candidate}_{workload.name}"
        command = [
            sys.executable, str(AUTOMATION),
            "--result-root", str(self.root / "runs"),
            "--dds", self.dds,
            "--label", label,
            "--modes", "opt12",
            "--controls", control,
            "--scenarios", workload.scenario,
            "--payload-kb", str(workload.payload_kb),
            "--hz", str(workload.hz),
            "--count", str(workload.count),
            "--max-samples", str(workload.count),
            "--qos-depth", str(min(self.qos_depth, workload.count)),
            "--trigger-after", str(trigger),
            "--case-duration-s", str(workload.disturbance_s),
            "--case-a-duration-s", str(workload.disturbance_s),
            "--case-a-loss-percent", str(workload.loss),
            "--bg-payload-kb", str(workload.bg_payload_kb),
            "--bg-hz", str(workload.bg_hz),
            "--bg-flows", str(workload.bg_flows),
            "--timeout-s", str(self.timeout_s),
            "--domain-base", str(140 + run_number % 70),
            "--link-rate-mbit", str(int(LINK_RATE_MBPS)),
            "--calm-rho-sample-multiplier", "2",
            "--calm-retry-signal", "rho_progress",
            "--calm-growth-rounds", "2",
            "--calm-ack-stall-ms", "500",
        ]
        if not self.storm_log:
            command.append("--no-storm-log")
        if parameters:
            command.extend(parameter_args(parameters))
        print(
            f"[DDS-SERVICE] {run_number} {stage}/{candidate} "
            f"{workload.name} lambda={workload.offered_mbps:.2f}Mbps",
            flush=True,
        )
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        print(completed.stdout, end="", flush=True)
        if completed.returncode != 0:
            raise RuntimeError(
                f"run failed ({completed.returncode}): {' '.join(command)}")
        result_dir = find_result(completed.stdout)
        summary = read_one(result_dir / "summary.csv")
        row: dict[str, Any] = {
            "stage": stage,
            "candidate": candidate,
            "workload": workload.name,
            "lambda_mbps": workload.offered_mbps,
            "lambda_class": workload.load_class,
            "score": 0.0,
            **(parameters or {}),
            **summary,
            "result_dir": str(result_dir),
        }
        row["score"] = score(row)
        self.rows.append(row)
        self.persist()
        return row


def select_candidate(rows: list[dict[str, Any]]) -> str:
    ranking: list[tuple[float, str]] = []
    for candidate in CANDIDATES:
        selected = [
            row for row in rows
            if row["stage"] == "screen" and row["candidate"] == candidate
        ]
        if len(selected) != len(SCREEN_WORKLOADS):
            continue
        # A median prevents one noisy loss realization from dominating, while
        # any incomplete run still dominates through its explicit penalty.
        ranking.append((statistics.median(score(row) for row in selected), candidate))
    if not ranking:
        raise RuntimeError("no complete dynamic candidate set")
    ranking.sort()
    return ranking[0][1]


def write_report(root: Path, rows: list[dict[str, Any]], selected: str) -> None:
    fields = (
        "stage", "candidate", "workload", "lambda_class", "lambda_mbps",
        "received_count", "requested_count", "sub_delay_mean_ms",
        "sub_delay_p95_ms", "sub_delay_max_ms", "sub_delay_std_ms",
        "sub_actual_hz", "rho_max_bytes", "calm_release_rate_min_mbps",
        "calm_release_rate_max_mbps", "calm_service_rate_final_mbps",
        "calm_service_success_rounds", "calm_service_failure_rounds", "score",
    )
    with (root / "comparison.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    validation = [row for row in rows if row["stage"] == "validation"]
    lines = [
        "# DDS-only CALM service-rate experiment",
        "",
        f"- Selected dynamic candidate: `{selected}`",
        f"- Reference netem service rate: `{LINK_RATE_MBPS:.0f} Mbps` (not provided to CALM)",
        "- All comparisons use OPT1+2; only CALM and its release-rate mode change.",
        "",
        "## Selected dimensionless parameters",
        "",
        "```json",
        json.dumps(CANDIDATES[selected], indent=2, sort_keys=True),
        "```",
        "",
        "## Validation",
        "",
        "| Workload | Load | Controller | Received | p95 delay (ms) | Jitter/std (ms) | Rx Hz | rho max (MiB) |",
        "|---|---:|---|---:|---:|---:|---:|---:|",
    ]
    for row in validation:
        lines.append(
            f"| {row['workload']} | {number(row, 'lambda_mbps'):.1f} | "
            f"{row['candidate']} | {int(number(row, 'received_count'))}/"
            f"{int(number(row, 'requested_count'))} | "
            f"{number(row, 'sub_delay_p95_ms'):.1f} | "
            f"{number(row, 'sub_delay_std_ms'):.1f} | "
            f"{number(row, 'sub_actual_hz'):.2f} | "
            f"{number(row, 'rho_max_bytes') / 1048576.0:.2f} |"
        )
    (root / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--result-root", type=Path, default=DEFAULT_RESULT_ROOT)
    parser.add_argument("--timeout-s", type=float, default=180.0)
    parser.add_argument("--dds", choices=("fastdds", "cyclonedds"), default="fastdds")
    parser.add_argument("--qos-depth", type=int, default=1)
    parser.add_argument(
        "--validation-only",
        action="store_true",
        help="Skip parameter screening and compare OPT1+2 Default with safe-window CALM.")
    parser.add_argument(
        "--workloads",
        help="Comma-separated validation workload names; default runs all.")
    parser.add_argument("--skip-validation", action="store_true")
    args = parser.parse_args()

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    if args.qos_depth <= 0:
        raise SystemExit("qos-depth must be positive")
    phase = "validation" if args.validation_only else "rate_tuning"
    root = args.result_root / f"CALM3_{args.dds}_dds_service_{phase}_{stamp}"
    (root / "runs").mkdir(parents=True, exist_ok=False)
    runner = Runner(
        root=root,
        timeout_s=args.timeout_s,
        dds=args.dds,
        qos_depth=args.qos_depth,
        storm_log=args.validation_only,
    )

    if args.validation_only:
        selected = "safe_window"
        selected_workloads = VALIDATION_WORKLOADS
        if args.workloads:
            requested = {
                item.strip() for item in args.workloads.split(",") if item.strip()}
            known = {workload.name for workload in VALIDATION_WORKLOADS}
            unknown = sorted(requested - known)
            if unknown:
                raise SystemExit(f"unknown workloads: {', '.join(unknown)}")
            selected_workloads = tuple(
                workload for workload in VALIDATION_WORKLOADS
                if workload.name in requested)
        for workload in selected_workloads:
            runner.run("validation", "default", workload, "default")
            runner.run(
                "validation", selected, workload, "calm", CANDIDATES[selected])
        write_report(root, runner.rows, selected)
        print(f"[DDS-SERVICE] selected={selected}")
        print(f"[DDS-SERVICE] results={root}")
        return 0

    # Keep the previous fixed-rate CALM in the screen as a performance anchor.
    for workload in SCREEN_WORKLOADS:
        runner.run("screen", "fixed", workload, "calm", {
            "calm-release-rate-mode": "fixed",
        })
        for candidate, parameters in CANDIDATES.items():
            runner.run("screen", candidate, workload, "calm", parameters)

    selected = select_candidate(runner.rows)
    (root / "selected_parameters.json").write_text(
        json.dumps({
            "candidate": selected,
            "parameters": CANDIDATES[selected],
            "link_rate_mbps_used_only_by_netem": LINK_RATE_MBPS,
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    if not args.skip_validation:
        for workload in VALIDATION_WORKLOADS:
            runner.run("validation", "default", workload, "default")
            runner.run("validation", "fixed", workload, "calm", {
                "calm-release-rate-mode": "fixed",
            })
            runner.run("validation", selected, workload, "calm", CANDIDATES[selected])

    write_report(root, runner.rows, selected)
    print(f"[DDS-SERVICE] selected={selected}")
    print(f"[DDS-SERVICE] results={root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
