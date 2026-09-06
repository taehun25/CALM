#!/usr/bin/env python3
"""Run focused two-host CALM experiments over a private Wi-Fi hotspot."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import shlex
import signal
import statistics
import subprocess
import threading
import time
from pathlib import Path


SCENARIOS = ("normal", "case_l", "case_d")
SSH_OPTIONS = (
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    "-o", "ConnectTimeout=5",
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
        self.trigger = threading.Event()
        self.publish_done = threading.Event()
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
        for line in self.proc.stdout:
            self._log.write(line)
            self._log.flush()
            if "[PUB_TRIGGER]" in line:
                self.trigger.set()
            if "messages have been published" in line:
                self.publish_done.set()

    def poll(self) -> int | None:
        return self.proc.poll()

    def wait(self, timeout: float) -> int | None:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def terminate(self, grace_s: float = 3.0) -> int:
        if self.poll() is None:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
            if self.wait(grace_s) is None:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
        code = self.proc.wait()
        self._thread.join(timeout=1.0)
        self._log.close()
        return code


def local_env(
    *,
    run_dir: Path,
    domain_id: int,
    calm_env: dict[str, str],
    metrics: bool,
) -> dict[str, str]:
    env = os.environ.copy()
    env["RMW_IMPLEMENTATION"] = "rmw_fastrtps_cpp"
    env["ROS_LOCALHOST_ONLY"] = "0"
    env["ROS_DOMAIN_ID"] = str(domain_id)
    env["ROS_LOG_DIR"] = str(run_dir / "ros_log")
    env["CALM_RESULT_DIR"] = str(run_dir)
    for name in (
        "FASTRTPS_DEFAULT_PROFILES_FILE",
        "FASTDDS_DEFAULT_PROFILES_FILE",
        "FASTDDS_BUILTIN_TRANSPORTS",
    ):
        env.pop(name, None)
    if metrics:
        env["FASTDDS_CALM_LOG_DIR"] = str(run_dir)
    else:
        env.pop("FASTDDS_CALM_LOG_DIR", None)
    env.update(calm_env)
    return env


def pub_args(
    *,
    idx: int,
    payload_bytes: int,
    count: int,
    max_samples: int,
    hz: int,
    topic: str,
    trigger_after: int,
    case_d_seconds: int = 0,
    case_d_enabled: bool = False,
    qos: str = "reliable",
) -> list[str]:
    return [
        "0", str(idx), str(payload_bytes), str(count), str(max_samples), "0",
        "image", str(1.0 / hz), str(case_d_seconds), str(hz), topic, qos,
        "1", "1" if case_d_enabled else "0", str(trigger_after),
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
) -> list[str]:
    return [
        "0", str(idx), str(payload_bytes), str(count), str(max_samples), "0",
        "0", "image", str(hz), topic, qos, "1", suffix,
    ]


def ssh_command(remote: str, shell_command: str) -> list[str]:
    return ["ssh", *SSH_OPTIONS, remote, "bash", "-lc", shell_command]


def remote_ros_command(
    *,
    remote: str,
    remote_ws: str,
    executable: str,
    arguments: list[str],
    env: dict[str, str],
    timeout_s: float,
) -> list[str]:
    exports = " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items())
    executable_path = f"{remote_ws}/install/calm_pretest_yw/lib/calm_pretest_yw/{executable}"
    command = (
        f"source /opt/ros/humble/setup.bash; "
        f"source {shlex.quote(remote_ws)}/install/setup.bash; "
        "unset FASTRTPS_DEFAULT_PROFILES_FILE FASTDDS_DEFAULT_PROFILES_FILE "
        "FASTDDS_BUILTIN_TRANSPORTS; "
        f"exec timeout --signal=TERM {timeout_s:g}s env {exports} "
        f"{shlex.quote(executable_path)} {shlex.join(arguments)}"
    )
    return ssh_command(remote, shlex.quote(command))


def snapshot_local(path: Path, interface: str) -> None:
    commands = [
        "date --iso-8601=ns",
        "hostname",
        "ip -br -4 addr",
        f"iw dev {shlex.quote(interface)} link",
        f"ip -s link show dev {shlex.quote(interface)}",
        f"tc -s qdisc show dev {shlex.quote(interface)}",
        "nstat -az",
        "sysctl net.core.rmem_default net.core.rmem_max "
        "net.core.wmem_default net.core.wmem_max",
    ]
    with path.open("w", encoding="utf-8") as stream:
        subprocess.run(
            ["bash", "-lc", "printf '\\n===== NEXT =====\\n'; ".join(commands)],
            stdout=stream,
            stderr=subprocess.STDOUT,
            check=False,
        )


def snapshot_remote(path: Path, remote: str, interface: str) -> None:
    command = (
        "date --iso-8601=ns; hostname; ip -br -4 addr; "
        f"iw dev {shlex.quote(interface)} link; "
        f"ip -s link show dev {shlex.quote(interface)}; "
        f"tc -s qdisc show dev {shlex.quote(interface)}; "
        "nstat -az; "
        "sysctl net.core.rmem_default net.core.rmem_max "
        "net.core.wmem_default net.core.wmem_max"
    )
    with path.open("w", encoding="utf-8") as stream:
        subprocess.run(
            ssh_command(remote, shlex.quote(command)),
            stdout=stream,
            stderr=subprocess.STDOUT,
            check=False,
        )


def select_main_reader(rows: list[dict[str, str]]) -> str:
    peaks: dict[str, int] = {}
    for row in rows:
        guid = row.get("reader_guid", "")
        rho = int(float(row.get("rho_bytes", "0") or 0))
        peaks[guid] = max(peaks.get(guid, 0), rho)
    return max(peaks, key=peaks.get) if peaks else ""


def summarize(run_dir: Path, scenario: str, requested_count: int, duration_s: float) -> dict[str, object]:
    pub_files = sorted(run_dir.glob("pub_*_qosreliable_*.csv"))
    sub_files = sorted((run_dir / "remote").glob("sub_*_qosreliable_*.csv"))
    pub_rows = read_csv(pub_files[-1]) if pub_files else []
    sub_rows = read_csv(sub_files[-1]) if sub_files else []
    pub_delays = [float(row["publish_delay_ms"]) for row in pub_rows]
    sub_delays = [float(row["delay_ms"]) for row in sub_rows]

    backlog_all = read_csv(run_dir / "calm_backlog.csv")
    main_reader = select_main_reader(backlog_all)
    backlog_rows = [row for row in backlog_all if row.get("reader_guid") == main_reader]
    budget_rows = [
        row for row in read_csv(run_dir / "calm_budget.csv")
        if row.get("reader_guid") == main_reader
    ]
    control_rows = [row for row in budget_rows if row.get("phase") == "control"]
    release_rows = [row for row in budget_rows if row.get("phase") != "control"]
    repair_release_rows = [
        row for row in release_rows
        if row.get("phase") in {"initial_release", "pacing_repair_release"}
    ]
    rho_peak = max((int(float(row["rho_bytes"])) for row in backlog_rows), default=0)
    release_total = sum(
        int(float(row.get("released_bytes", "0") or 0)) for row in repair_release_rows
    )
    rates = [float(row.get("repair_rate_mbps", "0") or 0) for row in control_rows]

    return {
        "scenario": scenario,
        "requested_count": requested_count,
        "published_count": len(pub_rows),
        "received_count": len(sub_rows),
        "receive_ratio": len(sub_rows) / requested_count if requested_count else 0.0,
        "duration_s": duration_s,
        "pub_delay_p95_ms": percentile(pub_delays, 0.95),
        "pub_delay_max_ms": max(pub_delays, default=0.0),
        "sub_delay_mean_ms": statistics.fmean(sub_delays) if sub_delays else 0.0,
        "sub_delay_p95_ms": percentile(sub_delays, 0.95),
        "sub_delay_max_ms": max(sub_delays, default=0.0),
        "rho_max_bytes": rho_peak,
        "requested_max_bytes": max(
            (int(float(row.get("requested_bytes", "0") or 0)) for row in backlog_rows),
            default=0,
        ),
        "held_new_max_bytes": max(
            (int(float(row.get("held_new_bytes", "0") or 0)) for row in budget_rows),
            default=0,
        ),
        "repair_release_total_bytes": release_total,
        "repair_release_max_bytes": max(
            (int(float(row.get("released_bytes", "0") or 0)) for row in repair_release_rows),
            default=0,
        ),
        "repair_amplification": release_total / rho_peak if rho_peak else 0.0,
        "control_count": len(control_rows),
        "repair_rate_min_mbps": min(rates, default=0.0),
        "repair_rate_max_mbps": max(rates, default=0.0),
        "main_reader_guid": main_reader,
    }


def copy_remote_results(remote: str, remote_run_dir: str, local_dir: Path) -> bool:
    local_dir.mkdir(parents=True, exist_ok=True)
    listing = subprocess.run(
        ssh_command(
            remote,
            shlex.quote(
                f"find {shlex.quote(remote_run_dir)} -maxdepth 1 -type f "
                "-name '*.csv' -printf '%f\\n'"
            ),
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    filenames = [line.strip() for line in listing.stdout.splitlines() if line.strip()]
    if listing.returncode != 0 or not filenames:
        (local_dir / "scp_error.txt").write_text(
            listing.stderr or "No remote CSV files found.\n",
            encoding="utf-8",
        )
        return False

    result = subprocess.run(
        [
            "scp", "-B", *SSH_OPTIONS,
            *(f"{remote}:{remote_run_dir}/{name}" for name in filenames),
            f"{local_dir}/",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        (local_dir / "scp_error.txt").write_text(result.stderr, encoding="utf-8")
        return False
    return True


def stop_remote_topic(remote: str, topic: str) -> None:
    pattern = f"dds_sub .* {topic} "
    command = f"pkill -f {shlex.quote(pattern)} || true"
    subprocess.run(
        ssh_command(remote, shlex.quote(command)),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def run_scenario(
    *,
    args: argparse.Namespace,
    batch_dir: Path,
    calm_env: dict[str, str],
    scenario: str,
    run_number: int,
) -> dict[str, object]:
    label = f"{args.label}_{scenario}"
    topic = f"calmhot_{args.domain_base}_{run_number}_{scenario}"
    run_dir = batch_dir / f"{run_number:02d}_{label}"
    remote_run_dir = f"{args.remote_result_root}/{batch_dir.name}/{run_number:02d}_{label}"
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "ros_log").mkdir(exist_ok=True)

    remote = f"{args.remote_user}@{args.remote_host}"
    domain_id = args.domain_base + run_number - 1
    subprocess.run(
        ssh_command(remote, shlex.quote(f"mkdir -p {shlex.quote(remote_run_dir)}")),
        check=True,
    )
    snapshot_local(run_dir / "local_before.txt", args.interface)
    snapshot_remote(run_dir / "remote_before.txt", remote, args.remote_interface)

    common_remote_env = {
        "RMW_IMPLEMENTATION": "rmw_fastrtps_cpp",
        "ROS_LOCALHOST_ONLY": "0",
        "ROS_DOMAIN_ID": str(domain_id),
        "ROS_LOG_DIR": f"{remote_run_dir}/ros_log",
        "CALM_RESULT_DIR": remote_run_dir,
        "CALM_SUB_INACTIVE_TIMEOUT_S": str(args.sub_inactive_timeout_s),
    }
    sub = ManagedProcess(
        remote_ros_command(
            remote=remote,
            remote_ws=args.remote_ws,
            executable="dds_sub",
            arguments=sub_args(
                idx=run_number,
                payload_bytes=args.payload_kb * 1024,
                count=args.count,
                max_samples=args.max_samples,
                hz=args.hz,
                topic=topic,
                suffix=label,
            ),
            env=common_remote_env,
            timeout_s=args.timeout_s + 10,
        ),
        os.environ.copy(),
        run_dir / "main_sub.log",
    )
    time.sleep(2.0)

    pub_environment = local_env(
        run_dir=run_dir,
        domain_id=domain_id,
        calm_env=calm_env,
        metrics=True,
    )
    pub = ManagedProcess(
        [
            str(Path(args.ws) / "install/calm_pretest_yw/lib/calm_pretest_yw/dds_pub"),
            *pub_args(
                idx=run_number,
                payload_bytes=args.payload_kb * 1024,
                count=args.count,
                max_samples=args.max_samples,
                hz=args.hz,
                topic=topic,
                trigger_after=args.trigger_after,
                case_d_seconds=round(args.case_duration_s),
                case_d_enabled=scenario == "case_d",
            ),
        ],
        pub_environment,
        run_dir / "main_pub.log",
    )

    started = time.monotonic()
    background: list[ManagedProcess] = []
    disturbance_done = scenario == "normal"
    while time.monotonic() - started < args.timeout_s:
        if not disturbance_done and pub.trigger.is_set():
            if scenario == "case_l":
                for flow in range(1, args.bg_flows + 1):
                    bg_topic = f"{topic}_bg{flow}"
                    bg_idx = run_number * 1000 + flow
                    background.append(
                        ManagedProcess(
                            remote_ros_command(
                                remote=remote,
                                remote_ws=args.remote_ws,
                                executable="dds_sub",
                                arguments=sub_args(
                                    idx=bg_idx,
                                    payload_bytes=args.bg_payload_kb * 1024,
                                    count=100000,
                                    max_samples=args.max_samples,
                                    hz=args.bg_hz,
                                    topic=bg_topic,
                                    suffix=f"{label}_bg",
                                    qos="best_effort",
                                ),
                                env=common_remote_env,
                                timeout_s=args.case_duration_s + 10,
                            ),
                            os.environ.copy(),
                            run_dir / f"bg{flow}_sub.log",
                        )
                    )
                time.sleep(1.5)
                bg_env = local_env(
                    run_dir=run_dir,
                    domain_id=domain_id,
                    calm_env=calm_env,
                    metrics=False,
                )
                for flow in range(1, args.bg_flows + 1):
                    bg_topic = f"{topic}_bg{flow}"
                    bg_idx = run_number * 1000 + flow
                    background.append(
                        ManagedProcess(
                            [
                                str(Path(args.ws) / "install/calm_pretest_yw/lib/calm_pretest_yw/dds_pub"),
                                *pub_args(
                                    idx=bg_idx,
                                    payload_bytes=args.bg_payload_kb * 1024,
                                    count=100000,
                                    max_samples=args.max_samples,
                                    hz=args.bg_hz,
                                    topic=bg_topic,
                                    trigger_after=100001,
                                    qos="best_effort",
                                ),
                            ],
                            bg_env,
                            run_dir / f"bg{flow}_pub.log",
                        )
                    )
                time.sleep(args.case_duration_s)
                for process in background:
                    process.terminate()
                background.clear()
                for flow in range(1, args.bg_flows + 1):
                    stop_remote_topic(remote, f"{topic}_bg{flow}")
            disturbance_done = True

        if pub.publish_done.is_set() and sub.wait(8.0) is not None:
            break
        if sub.poll() is not None:
            break
        time.sleep(0.1)

    for process in background:
        process.terminate()
    pub_exit = pub.terminate()
    sub_exit = sub.terminate()
    stop_remote_topic(remote, topic)
    elapsed = time.monotonic() - started

    copy_ok = copy_remote_results(remote, remote_run_dir, run_dir / "remote")
    snapshot_local(run_dir / "local_after.txt", args.interface)
    snapshot_remote(run_dir / "remote_after.txt", remote, args.remote_interface)
    row = summarize(run_dir, scenario, args.count, elapsed)
    row.update(
        {
            "label": args.label,
            "run_number": run_number,
            "domain_id": domain_id,
            "pub_exit": pub_exit,
            "sub_exit": sub_exit,
            "disturbance_done": disturbance_done,
            "remote_copy_ok": copy_ok,
            "run_dir": str(run_dir),
        }
    )
    return row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ws", default="/home/csi/ros2_ws")
    parser.add_argument("--result-root", default="/home/csi/ros2_ws/results/test_yw_2")
    parser.add_argument("--remote-host", default="172.20.10.3")
    parser.add_argument("--remote-user", default="csilab")
    parser.add_argument("--remote-ws", default="/home/csilab/ros2_ws")
    parser.add_argument("--remote-result-root", default="/home/csilab/ros2_ws/results/test_yw_2")
    parser.add_argument("--interface", default="wlp2s0")
    parser.add_argument("--remote-interface", default="wlp2s0")
    parser.add_argument("--label", default="candidate")
    parser.add_argument("--scenarios", default="normal,case_l,case_d")
    parser.add_argument("--payload-kb", type=int, default=1024)
    parser.add_argument("--hz", type=int, default=10)
    parser.add_argument("--count", type=int, default=180)
    parser.add_argument("--max-samples", type=int, default=400)
    parser.add_argument("--trigger-after", type=int, default=60)
    parser.add_argument("--case-duration-s", type=float, default=3.0)
    parser.add_argument("--bg-payload-kb", type=int, default=1024)
    parser.add_argument("--bg-hz", type=int, default=20)
    parser.add_argument("--bg-flows", type=int, default=2)
    parser.add_argument("--timeout-s", type=float, default=75.0)
    parser.add_argument("--sub-inactive-timeout-s", type=float, default=120.0)
    parser.add_argument("--domain-base", type=int, default=200)
    parser.add_argument("--controller", choices=("aimd", "pi", "pd"), default="aimd")
    parser.add_argument("--gamma", type=float, default=0.5)
    parser.add_argument("--alpha", type=float, default=4.0)
    parser.add_argument("--q-min", type=float, default=0.125)
    parser.add_argument("--pacing-ms", type=float, default=5.0)
    parser.add_argument("--fragment-bytes-per-period", type=int, default=16 * 1024)
    parser.add_argument("--fragment-period-ms", type=int, default=2)
    parser.add_argument("--max-budget-bytes", type=int, default=1024 * 1024)
    parser.add_argument("--max-inflight-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--min-repair-rate-mbps", type=float, default=8.0)
    parser.add_argument("--max-repair-rate-mbps", type=float, default=64.0)
    parser.add_argument("--held-new-rate-mbps", type=float, default=36.0)
    parser.add_argument("--kp", type=float, default=0.30)
    parser.add_argument("--ki", type=float, default=0.02)
    parser.add_argument("--kd", type=float, default=0.75)
    args = parser.parse_args()

    scenarios = [item.strip() for item in args.scenarios.split(",") if item.strip()]
    invalid = sorted(set(scenarios) - set(SCENARIOS))
    if invalid:
        parser.error(f"unsupported scenarios: {', '.join(invalid)}")
    if args.domain_base < 0 or args.domain_base + len(scenarios) - 1 > 232:
        parser.error("ROS domain IDs must be within 0..232")

    remote = f"{args.remote_user}@{args.remote_host}"
    subprocess.run(
        ["ssh", *SSH_OPTIONS, remote, "true"],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    if subprocess.run(
        ["sudo", "-n", "true"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode != 0 and "case_d" in scenarios:
        parser.error("Case D requires non-interactive sudo for tc netem")

    batch_dir = Path(args.result_root).resolve() / f"CALM3_hotspot_{args.label}_{timestamp()}"
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
        "FASTDDS_CALM_FRAGMENT_BYTES_PER_PERIOD": str(args.fragment_bytes_per_period),
        "FASTDDS_CALM_FRAGMENT_PERIOD_MS": str(args.fragment_period_ms),
    }

    rows: list[dict[str, object]] = []
    try:
        for index, scenario in enumerate(scenarios, start=1):
            print(f"[CALM-HOTSPOT] {args.label}: {scenario}", flush=True)
            row = run_scenario(
                args=args,
                batch_dir=batch_dir,
                calm_env=calm_env,
                scenario=scenario,
                run_number=index,
            )
            rows.append(row)
            print(
                f"[CALM-HOTSPOT] {scenario}: recv={row['received_count']}/{row['requested_count']} "
                f"p95={row['sub_delay_p95_ms']:.1f}ms rho={row['rho_max_bytes']} "
                f"repair={row['repair_release_total_bytes']}",
                flush=True,
            )
    finally:
        subprocess.run(
            ["sudo", "-n", "tc", "qdisc", "del", "dev", args.interface, "root"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )

    if rows:
        with (batch_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    print(f"[CALM-HOTSPOT] results: {batch_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
