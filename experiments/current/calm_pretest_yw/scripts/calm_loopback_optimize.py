#!/usr/bin/env python3
"""Run repeatable CALM Normal, Case L, and Case D loopback experiments."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import signal
import statistics
import subprocess
import threading
import time
from collections import Counter
from pathlib import Path


SCENARIOS = ("normal", "case_l", "case_d")


def configure_loopback_qdisc(
    *,
    rate_mbit: int,
    loss_percent: float,
    limit: int,
) -> None:
    subprocess.run(
        [
            "sudo", "tc", "qdisc", "replace", "dev", "lo", "root", "netem",
            "limit", str(limit),
            "rate", f"{rate_mbit}mbit",
            "loss", f"{loss_percent:g}%",
        ],
        check=True,
    )


def restore_loopback_qdisc() -> None:
    subprocess.run(
        ["sudo", "tc", "qdisc", "del", "dev", "lo", "root"],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        ["sudo", "tc", "qdisc", "add", "dev", "lo", "root", "netem", "limit", "1000"],
        check=True,
    )


def timestamp() -> str:
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


class ManagedProcess:
    def __init__(self, command: list[str], env: dict[str, str], log_path: Path):
        self.command = command
        self.log_path = log_path
        self.trigger = threading.Event()
        self.publish_done = threading.Event()
        self.lines: list[str] = []
        self._log = log_path.open("w", encoding="utf-8")
        self.proc = subprocess.Popen(
            command,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            start_new_session=True,
        )
        self._thread = threading.Thread(target=self._capture, daemon=True)
        self._thread.start()

    def _capture(self) -> None:
        assert self.proc.stdout is not None
        try:
            for line in self.proc.stdout:
                self.lines.append(line)
                self._log.write(line)
                self._log.flush()
                if "[PUB_TRIGGER]" in line:
                    self.trigger.set()
                if "messages have been published" in line:
                    self.publish_done.set()
        except (OSError, ValueError):
            # Process-group termination may close the pipe while the capture
            # thread is blocked in iteration.
            pass

    def poll(self) -> int | None:
        return self.proc.poll()

    def wait(self, timeout: float) -> int | None:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def signal_group(self, sig: signal.Signals) -> None:
        if self.poll() is None:
            os.killpg(os.getpgid(self.proc.pid), sig)

    def terminate(self, grace_s: float = 3.0) -> int:
        if self.poll() is None:
            self.signal_group(signal.SIGTERM)
            if self.wait(grace_s) is None:
                self.signal_group(signal.SIGKILL)
        code = self.proc.wait()
        self._thread.join(timeout=5.0)
        if self._thread.is_alive() and self.proc.stdout is not None:
            self.proc.stdout.close()
            self._thread.join(timeout=1.0)
        self._log.close()
        return code


def ros_command(package: str, executable: str, arguments: list[object]) -> list[str]:
    return ["ros2", "run", package, executable, *(str(arg) for arg in arguments)]


def process_env(
    *,
    ws: Path,
    run_dir: Path,
    domain_id: int,
    profile: Path,
    calm_env: dict[str, str],
    metrics: bool,
) -> dict[str, str]:
    env = os.environ.copy()
    env["RMW_IMPLEMENTATION"] = "rmw_fastrtps_cpp"
    # The Humble rmw_fastrtps localhost-only path unconditionally adds SHM after
    # loading XML. The XML transport whitelist already confines this harness to lo.
    env["ROS_LOCALHOST_ONLY"] = "0"
    env["ROS_DOMAIN_ID"] = str(domain_id)
    env["ROS_LOG_DIR"] = str(run_dir / "ros_log")
    env["CALM_RESULT_DIR"] = str(run_dir)
    env["LOOPBACK_TEST_PKG_ROOT"] = str(ws / "src" / "calm_pretest_yw")
    env["FASTRTPS_DEFAULT_PROFILES_FILE"] = str(profile)
    env.pop("FASTDDS_DEFAULT_PROFILES_FILE", None)
    env["RMW_FASTRTPS_USE_QOS_FROM_XML"] = "1"
    env.pop("FASTDDS_BUILTIN_TRANSPORTS", None)
    if metrics:
        env["FASTDDS_CALM_LOG_DIR"] = str(run_dir)
    else:
        env.pop("FASTDDS_CALM_LOG_DIR", None)
    env.update(calm_env)
    return env


def main_pub_args(
    *,
    idx: int,
    payload_bytes: int,
    count: int,
    max_samples: int,
    hz: int,
    topic: str,
    trigger_after: int,
    qos: str = "reliable",
    qos_depth: int = 1,
) -> list[object]:
    return [
        0, idx, payload_bytes, count, max_samples, 0, "image", 1.0 / hz,
        0, hz, topic, qos, qos_depth, 0, trigger_after,
    ]


def sub_args(
    *,
    idx: int,
    payload_bytes: int,
    count: int,
    max_samples: int,
    hz: int,
    topic: str,
    suffix: str,
    qos: str = "reliable",
    qos_depth: int = 1,
) -> list[object]:
    return [
        0, idx, payload_bytes, count, max_samples, 0, 0, "image", hz,
        topic, qos, qos_depth, suffix,
    ]


def start_background(
    *,
    env: dict[str, str],
    run_dir: Path,
    idx: int,
    payload_bytes: int,
    hz: int,
    count: int,
    max_samples: int,
    flows: int,
) -> list[ManagedProcess]:
    processes: list[ManagedProcess] = []
    bg_env = env.copy()
    bg_env.pop("FASTDDS_CALM_LOG_DIR", None)
    for flow in range(1, flows + 1):
        topic = f"bg{flow}"
        bg_idx = idx * 1000 + flow
        sub = ManagedProcess(
            ros_command(
                "calm_pretest_yw",
                "dds_sub",
                sub_args(
                    idx=bg_idx,
                    payload_bytes=payload_bytes,
                    count=count,
                    max_samples=max_samples,
                    hz=hz,
                    topic=topic,
                    suffix="loopback_bg",
                    qos="best_effort",
                ),
            ),
            bg_env,
            run_dir / f"{topic}_sub.log",
        )
        processes.append(sub)
    time.sleep(1.0)
    for flow in range(1, flows + 1):
        topic = f"bg{flow}"
        bg_idx = idx * 1000 + flow
        pub = ManagedProcess(
            ros_command(
                "calm_pretest_yw",
                "dds_pub",
                main_pub_args(
                    idx=bg_idx,
                    payload_bytes=payload_bytes,
                    count=count,
                    max_samples=max_samples,
                    hz=hz,
                    topic=topic,
                    trigger_after=count + 1,
                    qos="best_effort",
                ),
            ),
            bg_env,
            run_dir / f"{topic}_pub.log",
        )
        processes.append(pub)
    return processes


def select_main_reader(rows: list[dict[str, str]]) -> str:
    totals: dict[str, int] = {}
    for row in rows:
        guid = row.get("reader_guid", "")
        rho = int(float(row.get("rho_bytes", "0") or 0))
        totals[guid] = max(totals.get(guid, 0), rho)
    return max(totals, key=totals.get) if totals else ""


def summarize(run_dir: Path, scenario: str, requested_count: int, duration_s: float) -> dict[str, object]:
    pub_files = sorted(run_dir.glob("pub_*topicmain_*.csv"))
    sub_files = sorted(run_dir.glob("sub_*topicmain_*.csv"))
    pub_rows = read_csv(pub_files[-1]) if pub_files else []
    sub_rows = read_csv(sub_files[-1]) if sub_files else []
    pub_delays = [float(row["publish_delay_ms"]) for row in pub_rows]
    sub_delays = [float(row["delay_ms"]) for row in sub_rows]

    backlog_rows = read_csv(run_dir / "calm_backlog.csv")
    main_reader = select_main_reader(backlog_rows)
    backlog_rows = [row for row in backlog_rows if row.get("reader_guid") == main_reader]
    budget_rows = [
        row for row in read_csv(run_dir / "calm_budget.csv")
        if row.get("reader_guid") == main_reader
    ]
    control_rows = [row for row in budget_rows if row.get("phase") == "control"]
    release_rows = [row for row in budget_rows if row.get("phase") != "control"]

    return {
        "scenario": scenario,
        "requested_count": requested_count,
        "published_count": len(pub_rows),
        "received_count": len(sub_rows),
        "receive_ratio": len(sub_rows) / requested_count if requested_count else 0.0,
        "duration_s": duration_s,
        "pub_delay_mean_ms": statistics.fmean(pub_delays) if pub_delays else 0.0,
        "pub_delay_p95_ms": percentile(pub_delays, 0.95),
        "pub_delay_max_ms": max(pub_delays, default=0.0),
        "sub_delay_mean_ms": statistics.fmean(sub_delays) if sub_delays else 0.0,
        "sub_delay_p95_ms": percentile(sub_delays, 0.95),
        "sub_delay_max_ms": max(sub_delays, default=0.0),
        "rho_max_bytes": max((int(float(row["rho_bytes"])) for row in backlog_rows), default=0),
        "requested_max_bytes": max(
            (int(float(row.get("requested_bytes", "0") or 0)) for row in backlog_rows),
            default=0,
        ),
        "release_total_bytes": sum(int(float(row.get("released_bytes", "0") or 0)) for row in release_rows),
        "release_max_bytes": max(
            (int(float(row.get("released_bytes", "0") or 0)) for row in release_rows),
            default=0,
        ),
        "control_count": len(control_rows),
        "md_count": sum(
            row.get("action") in {
                "multiplicative_decrease",
                "repeated_nack_decrease",
                "pi_repeated_nack_brake",
                "pd_repeated_nack_brake",
            }
            for row in control_rows
        ),
        "ai_count": sum(
            row.get("action") in {"additive_increase", "pi_recovery", "pd_recovery"}
            for row in control_rows
        ),
        "hold_count": sum(row.get("action") == "hold" for row in control_rows),
    }


def run_scenario(
    *,
    ws: Path,
    batch_dir: Path,
    profile: Path,
    scenario: str,
    run_number: int,
    domain_id: int,
    payload_bytes: int,
    hz: int,
    count: int,
    max_samples: int,
    trigger_after: int,
    case_duration_s: float,
    bg_payload_bytes: int,
    bg_hz: int,
    bg_flows: int,
    timeout_s: float,
    calm_env: dict[str, str],
    label: str,
    qdisc_enabled: bool,
    link_rate_mbit: int,
    qdisc_limit: int,
) -> dict[str, object]:
    run_dir = batch_dir / f"{run_number:02d}_{label}_{scenario}"
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "ros_log").mkdir(exist_ok=True)
    if qdisc_enabled:
        configure_loopback_qdisc(
            rate_mbit=link_rate_mbit,
            loss_percent=0.0,
            limit=qdisc_limit,
        )
    pub_env = process_env(
        ws=ws,
        run_dir=run_dir,
        domain_id=domain_id,
        profile=profile,
        calm_env=calm_env,
        metrics=True,
    )
    sub_env = process_env(
        ws=ws,
        run_dir=run_dir,
        domain_id=domain_id,
        profile=profile,
        calm_env=calm_env,
        metrics=False,
    )

    topic = "main"
    suffix = f"{label}_{scenario}"
    sub = ManagedProcess(
        ros_command(
            "calm_pretest_yw",
            "dds_sub",
            sub_args(
                idx=run_number,
                payload_bytes=payload_bytes,
                count=count,
                max_samples=max_samples,
                hz=hz,
                topic=topic,
                suffix=suffix,
            ),
        ),
        sub_env,
        run_dir / "main_sub.log",
    )
    time.sleep(1.5)
    pub = ManagedProcess(
        ros_command(
            "calm_pretest_yw",
            "dds_pub",
            main_pub_args(
                idx=run_number,
                payload_bytes=payload_bytes,
                count=count,
                max_samples=max_samples,
                hz=hz,
                topic=topic,
                trigger_after=trigger_after,
            ),
        ),
        pub_env,
        run_dir / "main_pub.log",
    )

    started = time.monotonic()
    background: list[ManagedProcess] = []
    disturbance_done = scenario == "normal"
    trigger_deadline = started + max(30.0, trigger_after / hz + 20.0)
    while time.monotonic() - started < timeout_s:
        if not disturbance_done and pub.trigger.is_set():
            if scenario == "case_d":
                if qdisc_enabled:
                    configure_loopback_qdisc(
                        rate_mbit=link_rate_mbit,
                        loss_percent=100.0,
                        limit=qdisc_limit,
                    )
                else:
                    sub.signal_group(signal.SIGSTOP)
                time.sleep(case_duration_s)
                if qdisc_enabled:
                    configure_loopback_qdisc(
                        rate_mbit=link_rate_mbit,
                        loss_percent=0.0,
                        limit=qdisc_limit,
                    )
                else:
                    sub.signal_group(signal.SIGCONT)
            elif scenario == "case_l":
                background = start_background(
                    env=pub_env,
                    run_dir=run_dir,
                    idx=run_number,
                    payload_bytes=bg_payload_bytes,
                    hz=bg_hz,
                    count=100000,
                    max_samples=max_samples,
                    flows=bg_flows,
                )
                time.sleep(case_duration_s)
                for process in background:
                    process.terminate()
                background.clear()
            disturbance_done = True
        if not disturbance_done and time.monotonic() > trigger_deadline:
            disturbance_done = True
        if sub.poll() is not None:
            break
        if pub.publish_done.is_set() and time.monotonic() - started > count / hz + 10.0:
            if sub.wait(10.0) is not None:
                break
        time.sleep(0.1)

    for process in background:
        process.terminate()
    sub_exit = sub.terminate()
    pub_exit = pub.terminate()
    elapsed = time.monotonic() - started
    summary = summarize(run_dir, scenario, count, elapsed)
    summary.update(
        {
            "label": label,
            "run_number": run_number,
            "domain_id": domain_id,
            "pub_exit": pub_exit,
            "sub_exit": sub_exit,
            "disturbance_done": disturbance_done,
            "link_rate_mbit": link_rate_mbit if qdisc_enabled else 0,
            "run_dir": str(run_dir),
        }
    )
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ws", default="/home/csi/ros2_ws")
    parser.add_argument("--result-root", default="/home/csi/ros2_ws/results/test_yw_2")
    parser.add_argument("--label", default="candidate")
    parser.add_argument("--scenarios", default="normal,case_l,case_d")
    parser.add_argument("--payload-kb", type=int, default=4096)
    parser.add_argument("--hz", type=int, default=10)
    parser.add_argument("--count", type=int, default=300)
    parser.add_argument("--max-samples", type=int, default=400)
    parser.add_argument("--trigger-after", type=int, default=80)
    parser.add_argument("--case-duration-s", type=float, default=4.0)
    parser.add_argument("--bg-payload-kb", type=int, default=2048)
    parser.add_argument("--bg-hz", type=int, default=30)
    parser.add_argument("--bg-flows", type=int, default=2)
    parser.add_argument("--timeout-s", type=float, default=90.0)
    parser.add_argument("--domain-base", type=int, default=170)
    parser.add_argument("--gamma", type=float, default=0.75)
    parser.add_argument("--alpha", type=float, default=32.0)
    parser.add_argument("--q-min", type=float, default=0.10)
    parser.add_argument("--controller", default="pi")
    parser.add_argument("--pacing-ms", type=float, default=5.0)
    parser.add_argument("--max-budget-bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--max-inflight-bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--min-repair-rate-mbps", type=float, default=32.0)
    parser.add_argument("--max-repair-rate-mbps", type=float, default=320.0)
    parser.add_argument("--held-new-rate-mbps", type=float, default=800.0)
    parser.add_argument("--kp", type=float, default=0.30)
    parser.add_argument("--ki", type=float, default=0.02)
    parser.add_argument("--kd", type=float, default=0.75)
    parser.add_argument("--link-rate-mbit", type=int, default=1000)
    parser.add_argument("--qdisc-limit", type=int, default=10000)
    parser.add_argument("--no-qdisc", action="store_true")
    args = parser.parse_args()

    scenarios = [item.strip() for item in args.scenarios.split(",") if item.strip()]
    invalid = sorted(set(scenarios) - set(SCENARIOS))
    if invalid:
        parser.error(f"unsupported scenarios: {', '.join(invalid)}")
    if args.domain_base < 0 or args.domain_base + len(scenarios) - 1 > 232:
        parser.error("ROS domain IDs for all scenarios must be within 0..232")

    ws = Path(args.ws).resolve()
    profile = ws / "src" / "calm_pretest_yw" / "config" / "loopback_udp.xml"
    batch_dir = Path(args.result_root).resolve() / f"CALM3_loopback_{args.label}_{timestamp()}"
    batch_dir.mkdir(parents=True, exist_ok=True)
    with (batch_dir / "parameters.json").open("w", encoding="utf-8") as stream:
        json.dump(vars(args), stream, indent=2, sort_keys=True)
        stream.write("\n")
    calm_env = {
        "FASTDDS_CALM_GAMMA": str(args.gamma),
        "FASTDDS_CALM_ALPHA": str(args.alpha),
        "FASTDDS_CALM_Q_MIN": str(args.q_min),
        "FASTDDS_CALM_CONTROLLER": args.controller,
        "FASTDDS_CALM_PACING_MS": str(args.pacing_ms),
        "FASTDDS_CALM_MAX_BUDGET_BYTES": str(args.max_budget_bytes),
        "FASTDDS_CALM_MAX_INFLIGHT_BYTES": str(args.max_inflight_bytes),
        "FASTDDS_CALM_MIN_REPAIR_RATE_MBPS": str(args.min_repair_rate_mbps),
        "FASTDDS_CALM_MAX_REPAIR_RATE_MBPS": str(args.max_repair_rate_mbps),
        "FASTDDS_CALM_HELD_NEW_RATE_MBPS": str(args.held_new_rate_mbps),
        "FASTDDS_CALM_KP": str(args.kp),
        "FASTDDS_CALM_KI": str(args.ki),
        "FASTDDS_CALM_KD": str(args.kd),
    }

    rows: list[dict[str, object]] = []
    try:
        for offset, scenario in enumerate(scenarios):
            print(f"[CALM-LO] {args.label}: {scenario}", flush=True)
            row = run_scenario(
                ws=ws,
                batch_dir=batch_dir,
                profile=profile,
                scenario=scenario,
                run_number=offset + 1,
                domain_id=args.domain_base + offset,
                payload_bytes=args.payload_kb * 1024,
                hz=args.hz,
                count=args.count,
                max_samples=args.max_samples,
                trigger_after=args.trigger_after,
                case_duration_s=args.case_duration_s,
                bg_payload_bytes=args.bg_payload_kb * 1024,
                bg_hz=args.bg_hz,
                bg_flows=args.bg_flows,
                timeout_s=args.timeout_s,
                calm_env=calm_env,
                label=args.label,
                qdisc_enabled=not args.no_qdisc,
                link_rate_mbit=args.link_rate_mbit,
                qdisc_limit=args.qdisc_limit,
            )
            rows.append(row)
            print(
                f"[CALM-LO] {scenario}: recv={row['received_count']}/{row['requested_count']} "
                f"p95={row['sub_delay_p95_ms']:.1f}ms rho_max={row['rho_max_bytes']} "
                f"release_max={row['release_max_bytes']}",
                flush=True,
            )
    finally:
        if not args.no_qdisc:
            restore_loopback_qdisc()

    summary_path = batch_dir / "summary.csv"
    if rows:
        with summary_path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    print(f"[CALM-LO] results: {batch_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
