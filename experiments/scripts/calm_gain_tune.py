#!/usr/bin/env python3
"""Screen and validate CALM controller gains over shaped Ethernet."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path


CONTROLLERS = ("aimd", "pd", "pi", "pid")


@dataclass(frozen=True)
class Candidate:
    controller: str
    kp: float = 0.25
    ki: float = 0.02
    kd: float = 0.75
    gamma: float = 0.65
    alpha: float = 24.0

    @property
    def name(self) -> str:
        values = (
            f"{self.controller}_kp{self.kp:g}_ki{self.ki:g}_kd{self.kd:g}"
            f"_g{self.gamma:g}_a{self.alpha:g}"
        )
        return values.replace(".", "p")


def timestamp() -> str:
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def candidate_space(space: str) -> list[Candidate]:
    if space == "fine":
        candidates = [
            *(Candidate("aimd", gamma=gamma, alpha=12.0)
              for gamma in (0.55, 0.65, 0.75)),
            Candidate("aimd", gamma=0.65, alpha=6.0),
            Candidate("aimd", gamma=0.65, alpha=18.0),
            *(Candidate("pd", kp=kp, kd=1.50)
              for kp in (0.50, 0.70, 0.90)),
            Candidate("pd", kp=0.70, kd=1.00),
            Candidate("pd", kp=0.70, kd=2.00),
            *(Candidate("pi", kp=kp, ki=0.08)
              for kp in (0.20, 0.30, 0.45)),
            Candidate("pi", kp=0.30, ki=0.05),
            Candidate("pi", kp=0.30, ki=0.12),
            *(Candidate("pid", kp=kp, ki=0.03, kd=0.25)
              for kp in (0.30, 0.45, 0.60)),
            Candidate("pid", kp=0.45, ki=0.02, kd=0.25),
            Candidate("pid", kp=0.45, ki=0.05, kd=0.25),
            Candidate("pid", kp=0.45, ki=0.03, kd=0.10),
            Candidate("pid", kp=0.45, ki=0.03, kd=0.50),
        ]
        return list(dict.fromkeys(candidates))

    candidates: list[Candidate] = []
    for gamma in (0.45, 0.65, 0.85):
        for alpha in (12.0, 24.0, 48.0):
            candidates.append(Candidate("aimd", gamma=gamma, alpha=alpha))
    for kp in (0.10, 0.30, 0.70):
        for kd in (0.20, 0.75, 1.50):
            candidates.append(Candidate("pd", kp=kp, kd=kd))
    for kp in (0.10, 0.30, 0.70):
        for ki in (0.005, 0.02, 0.08):
            candidates.append(Candidate("pi", kp=kp, ki=ki))
    for kp in (0.15, 0.45):
        for ki in (0.005, 0.03):
            for kd in (0.25, 0.80, 1.50):
                candidates.append(Candidate("pid", kp=kp, ki=ki, kd=kd))
    return candidates


def read_summary(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def scenario_score(row: dict[str, str], payload_bytes: int, count: int) -> float:
    main_bytes = max(1.0, float(payload_bytes * count))
    duration = max(0.001, float(row["duration_s"]))
    receive_ratio = float(row["receive_ratio"])
    bad_exit = int(row["pub_exit"]) != 0 or int(row["sub_exit"]) != 0
    final_rho_ratio = float(row.get("rho_final_bytes", 0.0)) / main_bytes
    repair_ratio = float(row["repair_release_total_bytes"]) / main_bytes
    peak_ratio = float(row["rho_max_bytes"]) / main_bytes
    area_ratio = float(row.get("rho_area_byte_seconds", 0.0)) / (main_bytes * duration)
    held_ratio = float(row["held_new_max_bytes"]) / main_bytes
    p95_seconds = float(row["sub_delay_p95_ms"]) / 1000.0
    invalid_disturbance = (
        row["scenario"] == "case_l"
        and int(row.get("background_published_count", 0)) < 96
    )

    return (
        10000.0 * max(0.0, 1.0 - receive_ratio)
        + (1000.0 if bad_exit else 0.0)
        + (1000.0 if invalid_disturbance else 0.0)
        + 2.0 * final_rho_ratio
        + 4.0 * repair_ratio
        + 3.0 * peak_ratio
        + 2.0 * area_ratio
        + held_ratio
        + 0.25 * p95_seconds
        + 0.02 * duration
    )


def aggregate_score(rows: list[dict[str, str]], payload_bytes: int, count: int) -> float:
    weights = {"normal": 0.20, "case_l": 0.40, "case_d": 0.40}
    weighted = [
        scenario_score(row, payload_bytes, count) * weights.get(row["scenario"], 1.0)
        for row in rows
    ]
    weight_sum = sum(weights.get(row["scenario"], 1.0) for row in rows)
    return sum(weighted) / max(0.001, weight_sum)


def run_candidate(
    *,
    args: argparse.Namespace,
    candidate: Candidate,
    phase: str,
    index: int,
    scenarios: str,
    count: int,
    trigger_after: int,
) -> tuple[Path, list[dict[str, str]]]:
    label = f"tune_{args.run_id}_{phase}_{index:02d}_{candidate.name}"
    command = [
        str(args.runner),
        "--label", label,
        "--scenarios", scenarios,
        "--payload-kb", str(args.payload_kb),
        "--hz", str(args.hz),
        "--count", str(count),
        "--max-samples", str(args.max_samples),
        "--trigger-after", str(trigger_after),
        "--case-duration-s", str(args.case_duration_s),
        "--timeout-s", str(args.timeout_s),
        "--domain-base", str(args.domain_base),
        "--controller", candidate.controller,
        "--kp", str(candidate.kp),
        "--ki", str(candidate.ki),
        "--kd", str(candidate.kd),
        "--gamma", str(candidate.gamma),
        "--alpha", str(candidate.alpha),
    ]
    print(
        f"[CALM-TUNE] {phase} {index}: {candidate.name} scenarios={scenarios}",
        flush=True,
    )
    subprocess.run(command, check=True)
    matches = sorted(
        args.result_root.glob(f"CALM3_ethernet_{label}_*"),
        key=lambda path: path.stat().st_mtime_ns,
    )
    if not matches:
        raise RuntimeError(f"result directory not found for {label}")
    result_dir = matches[-1]
    return result_dir, read_summary(result_dir / "summary.csv")


def write_rows(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def load_validation_candidates(path: Path, top_per_controller: int) -> list[Candidate]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    selected: list[Candidate] = []
    for controller in CONTROLLERS:
        controller_rows = sorted(
            (row for row in rows if row["controller"] == controller),
            key=lambda row: float(row["score"]),
        )
        for row in controller_rows[:top_per_controller]:
            selected.append(
                Candidate(
                    controller=controller,
                    kp=float(row["kp"]),
                    ki=float(row["ki"]),
                    kd=float(row["kd"]),
                    gamma=float(row["gamma"]),
                    alpha=float(row["alpha"]),
                )
            )
    return selected


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("screen", "validate", "confirm"), required=True)
    parser.add_argument(
        "--runner",
        type=Path,
        default=Path("/home/csi/ros2_ws/src/calm_pretest_yw/scripts/calm_ethernet_optimize.py"),
    )
    parser.add_argument(
        "--result-root",
        type=Path,
        default=Path("/home/csi/ros2_ws/results/test_yw_2"),
    )
    parser.add_argument("--run-id", default=timestamp())
    parser.add_argument("--space", choices=("coarse", "fine"), default="coarse")
    parser.add_argument("--input", type=Path)
    parser.add_argument("--top-per-controller", type=int, default=2)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--payload-kb", type=int, default=1024)
    parser.add_argument("--hz", type=int, default=10)
    parser.add_argument("--max-samples", type=int, default=400)
    parser.add_argument("--case-duration-s", type=float, default=3.0)
    parser.add_argument("--timeout-s", type=float, default=55.0)
    parser.add_argument("--domain-base", type=int, default=120)
    args = parser.parse_args()

    args.result_root = args.result_root.resolve()
    args.runner = args.runner.resolve()
    master_dir = args.result_root / f"CALM3_gain_tuning_{args.run_id}_{args.phase}"
    master_dir.mkdir(parents=True, exist_ok=True)

    if args.phase == "screen":
        candidates = candidate_space(args.space)
        scenarios = "case_l"
        count = 75
        trigger_after = 20
    elif args.phase == "validate":
        if args.input is None:
            parser.error("--input screening.csv is required for validation")
        candidates = load_validation_candidates(args.input, args.top_per_controller)
        scenarios = "normal,case_l,case_d"
        count = 120
        trigger_after = 30
    else:
        if args.input is None:
            parser.error("--input validation ranking.csv is required for confirmation")
        finalists = load_validation_candidates(args.input, 1)
        candidates = [
            candidate
            for candidate in finalists
            for _ in range(args.repeats)
        ]
        scenarios = "normal,case_l,case_d"
        count = 180
        trigger_after = 50
    if args.limit > 0:
        candidates = candidates[:args.limit]

    objective = {
        "hard_priorities": ["receive_ratio", "process_exit"],
        "soft_weights": {
            "rho_final_ratio": 2.0,
            "repair_ratio": 4.0,
            "rho_peak_ratio": 3.0,
            "rho_area_ratio": 2.0,
            "held_new_ratio": 1.0,
            "p95_delay_seconds": 0.25,
            "duration_seconds": 0.02,
        },
        "phase": args.phase,
        "scenarios": scenarios,
        "payload_kb": args.payload_kb,
        "hz": args.hz,
        "count": count,
        "trigger_after": trigger_after,
    }
    (master_dir / "objective.json").write_text(
        json.dumps(objective, indent=2) + "\n",
        encoding="utf-8",
    )

    output_rows: list[dict[str, object]] = []
    for index, candidate in enumerate(candidates, start=1):
        try:
            result_dir, summary_rows = run_candidate(
                args=args,
                candidate=candidate,
                phase=args.phase,
                index=index,
                scenarios=scenarios,
                count=count,
                trigger_after=trigger_after,
            )
            score = aggregate_score(
                summary_rows,
                args.payload_kb * 1024,
                count,
            )
            status = "ok"
        except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
            result_dir = Path()
            summary_rows = []
            score = 1_000_000.0
            status = f"failed: {error}"
        row: dict[str, object] = {
            **asdict(candidate),
            "candidate": candidate.name,
            "score": score,
            "status": status,
            "result_dir": str(result_dir),
        }
        for summary in summary_rows:
            scenario = summary["scenario"]
            for key in (
                "receive_ratio",
                "background_published_count",
                "duration_s",
                "sub_delay_p95_ms",
                "rho_max_bytes",
                "rho_final_bytes",
                "rho_area_byte_seconds",
                "held_new_max_bytes",
                "repair_release_total_bytes",
                "repair_amplification",
                "pub_exit",
                "sub_exit",
            ):
                row[f"{scenario}_{key}"] = summary.get(key, "")
        output_rows.append(row)
        output_rows.sort(key=lambda item: float(item["score"]))
        write_rows(master_dir / "ranking.csv", output_rows)
        print(f"[CALM-TUNE] score={score:.6f} status={status}", flush=True)

    print(f"[CALM-TUNE] ranking: {master_dir / 'ranking.csv'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
