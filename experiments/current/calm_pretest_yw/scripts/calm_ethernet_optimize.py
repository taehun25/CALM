#!/usr/bin/env python3
"""Run Stock/MTU-safe/CALM sweeps over a private Ethernet or Wi-Fi link."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import os
import shlex
import signal
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path


SCENARIOS = ("normal", "case_l", "case_d")
EXPERIMENT_MODES = {
    "stock_default": {
        "variant": "A",
        "calm_enabled": 0,
        "mtu_safe": False,
    },
    "mtu_default": {
        "variant": "B",
        "calm_enabled": 0,
        "mtu_safe": True,
    },
    "mtu_calm": {
        "variant": "C",
        "calm_enabled": 1,
        "mtu_safe": True,
    },
}
NETWORK_PRESETS = {
    "ethernet": {
        "local_ip": "192.168.50.1",
        "remote_ip": "192.168.50.2",
        "interface": "enp1s0",
        "local_profile": "ethernet_pub.xml",
        "remote_profile": "ethernet_sub.xml",
        "expected_ssid": "",
    },
    "wifi": {
        "local_ip": "192.168.0.2",
        "remote_ip": "192.168.0.3",
        "interface": "wlp2s0",
        "local_profile": "wifi_pub.xml",
        "remote_profile": "wifi_sub.xml",
        "expected_ssid": "iptime5GAX3000SM",
    },
}
SSH_OPTIONS = (
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    "-o", "ConnectTimeout=5",
)


def resolve_network_settings(args: argparse.Namespace) -> None:
    preset = NETWORK_PRESETS[args.network]
    args.local_ip = args.local_ip or preset["local_ip"]
    args.remote_host = args.remote_host or preset["remote_ip"]
    args.remote_management_host = args.remote_management_host or args.remote_host
    args.interface = args.interface or preset["interface"]
    args.remote_interface = args.remote_interface or preset["interface"]
    args.local_profile = args.local_profile or str(
        Path(args.ws) / "src/calm_pretest_yw/config" / preset["local_profile"]
    )
    args.remote_profile = args.remote_profile or (
        f"{args.remote_ws}/src/calm_pretest_yw/config/{preset['remote_profile']}"
    )
    if args.expected_ssid is None:
        args.expected_ssid = preset["expected_ssid"]
    args.shaping_enabled = (
        args.shaping == "on" or
        (args.shaping == "auto" and args.network == "ethernet")
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


def parse_integer_list(raw: str, name: str, allowed: set[int] | None = None) -> list[int]:
    values: list[int] = []
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        try:
            value = int(item)
        except ValueError as error:
            raise ValueError(f"{name} contains a non-integer value: {item}") from error
        if value <= 0 and allowed is None:
            raise ValueError(f"{name} values must be positive: {value}")
        if allowed is not None and value not in allowed:
            choices = ", ".join(str(choice) for choice in sorted(allowed))
            raise ValueError(f"{name} values must be one of {choices}: {value}")
        if value not in values:
            values.append(value)
    if not values:
        raise ValueError(f"{name} must contain at least one value")
    return values


def parse_choice_list(raw: str, name: str, allowed: set[str]) -> list[str]:
    values: list[str] = []
    for item in raw.split(","):
        value = item.strip().lower()
        if not value:
            continue
        if value not in allowed:
            choices = ", ".join(sorted(allowed))
            raise ValueError(f"{name} values must be one of {choices}: {value}")
        if value not in values:
            values.append(value)
    if not values:
        raise ValueError(f"{name} must contain at least one value")
    return values


def apply_experiment_mode(args: argparse.Namespace) -> None:
    if not args.experiment_mode:
        args.experiment_mode = "mtu_calm" if args.calm_enabled else "stock_default"
    mode = EXPERIMENT_MODES[args.experiment_mode]
    args.experiment_variant = mode["variant"]
    args.calm_enabled = mode["calm_enabled"]
    args.mtu_safe = mode["mtu_safe"]
    args.rtps_message_size_limit = args.max_rtps_message_size if args.mtu_safe else 0


def atomic_write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
    temporary.replace(path)


class ManagedProcess:
    def __init__(self, command: list[str], env: dict[str, str], log_path: Path):
        self.trigger = threading.Event()
        self.publish_done = threading.Event()
        self.publish_started = threading.Event()
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
            if "Received subscriber ready message. Starting publish timer." in line:
                self.publish_started.set()
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
    profile: str,
    interface: str,
    netem_rate_mbit: float,
    netem_delay_ms: float,
    netem_jitter_ms: float,
    netem_loss_pct: float,
) -> dict[str, str]:
    env = os.environ.copy()
    env["RMW_IMPLEMENTATION"] = "rmw_fastrtps_cpp"
    env["ROS_LOCALHOST_ONLY"] = "0"
    env["ROS_DOMAIN_ID"] = str(domain_id)
    env["ROS_LOG_DIR"] = str(run_dir / "ros_log")
    env["CALM_RESULT_DIR"] = str(run_dir)
    for name in ("FASTDDS_DEFAULT_PROFILES_FILE", "FASTDDS_BUILTIN_TRANSPORTS"):
        env.pop(name, None)
    env["FASTRTPS_DEFAULT_PROFILES_FILE"] = profile
    env["CALM_NET_INTERFACE"] = interface
    env["CALM_NETEM_RATE_MBIT"] = str(netem_rate_mbit)
    env["CALM_NETEM_DELAY_MS"] = str(netem_delay_ms)
    env["CALM_NETEM_JITTER_MS"] = str(netem_jitter_ms)
    env["CALM_NETEM_BASE_LOSS_PCT"] = str(netem_loss_pct)
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
        "unset FASTDDS_DEFAULT_PROFILES_FILE FASTDDS_BUILTIN_TRANSPORTS; "
        f"exec timeout --signal=TERM {timeout_s:g}s env {exports} "
        f"{shlex.quote(executable_path)} {shlex.join(arguments)}"
    )
    return ssh_command(remote, shlex.quote(command))


def snapshot_local(path: Path, interface: str) -> None:
    link_details = (
        f"iw dev {shlex.quote(interface)} link"
        if interface.startswith("wl")
        else f"ethtool {shlex.quote(interface)}"
    )
    commands = [
        "date --iso-8601=ns",
        "hostname",
        "ip -br -4 addr",
        link_details,
        f"ip -s link show dev {shlex.quote(interface)}",
        f"tc -s qdisc show dev {shlex.quote(interface)}",
        "nstat -az",
        "sysctl net.core.rmem_default net.core.rmem_max "
        "net.core.wmem_default net.core.wmem_max",
    ]
    with path.open("w", encoding="utf-8") as stream:
        subprocess.run(
            ["bash", "-lc", "; printf '\\n===== NEXT =====\\n'; ".join(commands)],
            stdout=stream,
            stderr=subprocess.STDOUT,
            check=False,
        )


def snapshot_remote(path: Path, remote: str, interface: str) -> None:
    link_details = (
        f"iw dev {shlex.quote(interface)} link"
        if interface.startswith("wl")
        else f"ethtool {shlex.quote(interface)}"
    )
    command = (
        "date --iso-8601=ns; hostname; ip -br -4 addr; "
        f"{link_details}; "
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


def netem_arguments(
    args: argparse.Namespace,
    interface: str,
    loss_pct: float | None = None,
) -> list[str]:
    command = [
        "tc", "qdisc", "replace", "dev", interface, "root", "netem",
        "limit", str(args.netem_limit_packets),
    ]
    if args.netem_delay_ms > 0:
        command += ["delay", f"{args.netem_delay_ms:g}ms"]
        if args.netem_jitter_ms > 0:
            command += [
                f"{args.netem_jitter_ms:g}ms", "distribution", "normal",
            ]
    effective_loss_pct = args.netem_loss_pct if loss_pct is None else loss_pct
    if effective_loss_pct > 0:
        command += ["loss", f"{effective_loss_pct:g}%"]
    if args.netem_rate_mbit > 0:
        command += ["rate", f"{args.netem_rate_mbit:g}mbit"]
    return command


def apply_local_netem(
    args: argparse.Namespace,
    loss_pct: float | None = None,
) -> None:
    subprocess.run(
        ["sudo", "-n", *netem_arguments(args, args.interface, loss_pct)],
        check=True,
    )


def apply_remote_netem(
    args: argparse.Namespace,
    remote: str,
    loss_pct: float | None = None,
) -> None:
    command = shlex.join(
        ["sudo", "-n", *netem_arguments(args, args.remote_interface, loss_pct)]
    )
    subprocess.run(ssh_command(remote, shlex.quote(command)), check=True)


def clear_local_netem(interface: str) -> None:
    subprocess.run(
        ["sudo", "-n", "tc", "qdisc", "del", "dev", interface, "root"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def clear_remote_netem(remote: str, interface: str) -> None:
    command = f"sudo -n tc qdisc del dev {shlex.quote(interface)} root || true"
    subprocess.run(
        ssh_command(remote, shlex.quote(command)),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def restore_local_baseline(args: argparse.Namespace) -> None:
    if args.shaping_enabled:
        apply_local_netem(args)
    else:
        clear_local_netem(args.interface)


def restore_remote_baseline(args: argparse.Namespace, remote: str) -> None:
    if args.shaping_enabled:
        apply_remote_netem(args, remote)
    else:
        clear_remote_netem(remote, args.remote_interface)


def append_disturbance_event(
    path: Path,
    scenario: str,
    cycle: int,
    event: str,
    detail: str,
) -> None:
    write_header = not path.exists()
    with path.open("a", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        if write_header:
            writer.writerow(("time_ns", "scenario", "cycle", "event", "detail"))
        writer.writerow((time.time_ns(), scenario, cycle, event, detail))


def run_case_d_cycles(
    args: argparse.Namespace,
    remote: str,
    event_path: Path,
) -> bool:
    completed = True
    for cycle in range(1, args.case_d_cycles + 1):
        append_disturbance_event(
            event_path,
            "case_d",
            cycle,
            "outage_apply",
            (
                f"loss={args.case_d_loss_pct:g}% "
                f"bidirectional={args.case_d_bidirectional}"
            ),
        )
        outage_started = 0.0
        try:
            apply_local_netem(args, args.case_d_loss_pct)
            if args.case_d_bidirectional:
                apply_remote_netem(args, remote, args.case_d_loss_pct)
            outage_started = time.monotonic()
            append_disturbance_event(
                event_path,
                "case_d",
                cycle,
                "outage_start",
                (
                    f"loss={args.case_d_loss_pct:g}% "
                    f"bidirectional={args.case_d_bidirectional}"
                ),
            )
            time.sleep(args.case_duration_s)
        except subprocess.CalledProcessError:
            completed = False
        finally:
            restore_local_baseline(args)
            if args.case_d_bidirectional:
                restore_remote_baseline(args, remote)
            append_disturbance_event(
                event_path,
                "case_d",
                cycle,
                "outage_end",
                (
                    f"requested_s={args.case_duration_s:g} "
                    f"actual_s={max(0.0, time.monotonic() - outage_started):.3f}"
                    if outage_started else "qdisc_apply_failed"
                ),
            )

        if cycle < args.case_d_cycles and args.case_d_recovery_s > 0:
            append_disturbance_event(
                event_path,
                "case_d",
                cycle,
                "recovery_window",
                f"duration_s={args.case_d_recovery_s:g}",
            )
            time.sleep(args.case_d_recovery_s)
    return completed


def verify_selected_network(args: argparse.Namespace, remote: str) -> None:
    local_address = subprocess.run(
        ["ip", "-o", "-4", "addr", "show", "dev", args.interface],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if (
        local_address.returncode != 0 or
        f"inet {args.local_ip}/" not in local_address.stdout
    ):
        raise ValueError(
            f"{args.network} requires {args.local_ip} on {args.interface}; "
            f"current={local_address.stdout.strip() or '(none)'}"
        )

    route = subprocess.run(
        ["ip", "route", "get", args.remote_host],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if (
        route.returncode != 0 or
        f"dev {args.interface}" not in route.stdout or
        f"src {args.local_ip}" not in route.stdout
    ):
        raise ValueError(
            f"route to {args.remote_host} is not pinned to "
            f"{args.interface}/{args.local_ip}: {route.stdout.strip()}"
        )

    if args.expected_ssid:
        local_link = subprocess.run(
            ["iw", "dev", args.interface, "link"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if (
            local_link.returncode != 0 or
            f"SSID: {args.expected_ssid}" not in local_link.stdout
        ):
            raise ValueError(
                f"{args.interface} is not connected to SSID {args.expected_ssid!r}"
            )

    checks = [
        "set -e",
        (
            f"ip -o -4 addr show dev {shlex.quote(args.remote_interface)} "
            f"| grep -Fq {shlex.quote(f'inet {args.remote_host}/')}"
        ),
        (
            f"ip route get {shlex.quote(args.local_ip)} "
            f"| grep -Fq {shlex.quote(f'dev {args.remote_interface}')}"
        ),
        f"test -f {shlex.quote(args.remote_profile)}",
    ]
    if args.expected_ssid:
        checks.append(
            f"iw dev {shlex.quote(args.remote_interface)} link "
            f"| grep -Fq {shlex.quote(f'SSID: {args.expected_ssid}')}"
        )
    result = subprocess.run(
        ssh_command(remote, shlex.quote("; ".join(checks))),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ValueError(
            f"remote {args.network} interface/profile validation failed: "
            f"{detail or 'IP, route, SSID, or profile mismatch'}"
        )


def preflight_environment(args: argparse.Namespace, scenarios: list[str]) -> str:
    if not Path(args.local_profile).is_file():
        raise ValueError(f"local DDS profile not found: {args.local_profile}")

    remote = f"{args.remote_user}@{args.remote_management_host}"
    verify_selected_network(args, remote)

    local_sudo_required = args.shaping_enabled or "case_d" in scenarios
    if local_sudo_required and subprocess.run(
        ["sudo", "-n", "true"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode != 0:
        raise ValueError(
            "local non-interactive sudo is required for baseline shaping or CASE D"
        )

    remote_sudo_required = args.shaping_enabled or (
        "case_d" in scenarios and args.case_d_bidirectional
    )
    if remote_sudo_required and subprocess.run(
        ssh_command(remote, shlex.quote("sudo -n true")),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode != 0:
        raise ValueError(
            "remote non-interactive sudo is required for shaping or bidirectional CASE D"
        )
    return remote


def select_main_reader(rows: list[dict[str, str]]) -> str:
    peaks: dict[str, int] = {}
    for row in rows:
        guid = row.get("reader_guid", "")
        rho = int(float(row.get("rho_bytes", "0") or 0))
        peaks[guid] = max(peaks.get(guid, 0), rho)
    return max(peaks, key=peaks.get) if peaks else ""


def select_observer_reader(rows: list[dict[str, str]]) -> str:
    scores: dict[str, tuple[int, int]] = {}
    for row in rows:
        guid = row.get("reader_guid", "")
        unique_bytes = int(float(row.get("unique_repair_bytes_total", "0") or 0))
        rho = int(float(row.get("rho_bytes", "0") or 0))
        scores[guid] = max(scores.get(guid, (0, 0)), (unique_bytes, rho))
    return max(scores, key=scores.get) if scores else ""


def summarize(
    run_dir: Path,
    scenario: str,
    requested_count: int,
    duration_s: float,
    payload_bytes: int,
) -> dict[str, object]:
    pub_files = sorted(run_dir.glob("pub_*_qosreliable_*.csv"))
    sub_files = sorted((run_dir / "remote").glob("sub_*_qosreliable_*.csv"))
    pub_rows = read_csv(pub_files[-1]) if pub_files else []
    sub_rows = read_csv(sub_files[-1]) if sub_files else []
    background_published_count = sum(
        len(read_csv(path))
        for path in run_dir.glob("pub_*_qosbest_effort_*.csv")
    )
    pub_delays = [float(row["publish_delay_ms"]) for row in pub_rows]
    sub_delays = [float(row["delay_ms"]) for row in sub_rows]

    backlog_all = read_csv(run_dir / "calm_backlog.csv")
    main_reader = select_main_reader(backlog_all)
    backlog_rows = [row for row in backlog_all if row.get("reader_guid") == main_reader]
    budget_rows = [
        row for row in read_csv(run_dir / "calm_budget.csv")
        if row.get("reader_guid") == main_reader
    ]
    observer_all = read_csv(run_dir / "calm_observer.csv")
    observer_reader = select_observer_reader(observer_all)
    observer_rows = [
        row for row in observer_all if row.get("reader_guid") == observer_reader
    ]
    control_rows = [row for row in budget_rows if row.get("phase") == "control"]
    release_rows = [row for row in budget_rows if row.get("phase") != "control"]
    repair_release_rows = [
        row for row in release_rows
        if row.get("phase") in {"initial_release", "pacing_repair_release"}
    ]
    new_release_rows = [
        row for row in release_rows
        if row.get("phase") == "pacing_new_release"
    ]
    rho_peak = max((int(float(row["rho_bytes"])) for row in backlog_rows), default=0)
    rho_final = int(float(backlog_rows[-1]["rho_bytes"])) if backlog_rows else 0
    rho_area_byte_seconds = 0.0
    for previous, current in zip(backlog_rows, backlog_rows[1:]):
        previous_time = int(previous["time_ns"])
        current_time = int(current["time_ns"])
        elapsed = max(0.0, (current_time - previous_time) / 1_000_000_000.0)
        previous_rho = int(float(previous["rho_bytes"]))
        current_rho = int(float(current["rho_bytes"]))
        rho_area_byte_seconds += (previous_rho + current_rho) * 0.5 * elapsed
    release_total = sum(
        int(float(row.get("released_bytes", "0") or 0)) for row in repair_release_rows
    )
    published_payload_bytes = len(pub_rows) * payload_bytes
    observer_last = observer_rows[-1] if observer_rows else {}
    observer_nack_bytes = int(
        float(observer_last.get("nack_bytes_total", "0") or 0)
    )
    observer_unique_repair_bytes = int(
        float(observer_last.get("unique_repair_bytes_total", "0") or 0)
    )
    observer_repeated_nack_bytes = int(
        float(observer_last.get("repeated_nack_bytes_total", "0") or 0)
    )
    observer_release_bytes = int(
        float(observer_last.get("repair_release_bytes_total", "0") or 0)
    )
    observer_repair_amplification = (
        observer_release_bytes / observer_unique_repair_bytes
        if observer_unique_repair_bytes else 0.0
    )
    observer_repeat_ratio = (
        observer_repeated_nack_bytes / observer_nack_bytes
        if observer_nack_bytes else 0.0
    )
    observer_repair_overhead = (
        observer_release_bytes / published_payload_bytes
        if published_payload_bytes else 0.0
    )
    receive_ratio = len(sub_rows) / requested_count if requested_count else 0.0
    sub_delay_p95_ms = percentile(sub_delays, 0.95)
    observer_rho_final = int(
        float(observer_last.get("rho_bytes", "0") or 0)
    )
    feedback_amplification = (
        observer_unique_repair_bytes > 0 and
        observer_repair_amplification >= 2.0 and
        observer_repeat_ratio >= 0.25 and
        observer_repair_overhead >= 0.50
    )
    performance_collapse = (
        receive_ratio < 0.99 or
        sub_delay_p95_ms >= 10_000.0 or
        (
            observer_unique_repair_bytes > 0 and
            observer_rho_final / observer_unique_repair_bytes >= 0.10
        )
    )
    storm_candidate = feedback_amplification and performance_collapse
    rates = [float(row.get("repair_rate_mbps", "0") or 0) for row in control_rows]
    path_rates = [float(row.get("path_rate_mbps", "0") or 0) for row in budget_rows]
    decrease_count = sum(
        "decrease" in row.get("action", "") for row in control_rows
    )
    increase_count = sum(
        "increase" in row.get("action", "") for row in control_rows
    )

    return {
        "scenario": scenario,
        "requested_count": requested_count,
        "published_count": len(pub_rows),
        "received_count": len(sub_rows),
        "receive_ratio": receive_ratio,
        "background_published_count": background_published_count,
        "duration_s": duration_s,
        "pub_delay_p95_ms": percentile(pub_delays, 0.95),
        "pub_delay_max_ms": max(pub_delays, default=0.0),
        "sub_delay_mean_ms": statistics.fmean(sub_delays) if sub_delays else 0.0,
        "sub_delay_p95_ms": sub_delay_p95_ms,
        "sub_delay_max_ms": max(sub_delays, default=0.0),
        "rho_max_bytes": rho_peak,
        "rho_final_bytes": rho_final,
        "rho_area_byte_seconds": rho_area_byte_seconds,
        "requested_max_bytes": max(
            (int(float(row.get("requested_bytes", "0") or 0)) for row in backlog_rows),
            default=0,
        ),
        "scheduled_repair_max_bytes": max(
            (int(float(row.get("scheduled_repair_bytes", "0") or 0)) for row in budget_rows),
            default=0,
        ),
        "held_new_max_bytes": max(
            (int(float(row.get("held_new_bytes", "0") or 0)) for row in budget_rows),
            default=0,
        ),
        "queued_new_max_bytes": max(
            (int(float(row.get("queued_new_bytes", "0") or 0)) for row in budget_rows),
            default=0,
        ),
        "new_release_count": len(new_release_rows),
        "new_release_total_bytes": sum(
            int(float(row.get("released_bytes", "0") or 0)) for row in new_release_rows
        ),
        "repair_release_total_bytes": release_total,
        "repair_release_max_bytes": max(
            (int(float(row.get("released_bytes", "0") or 0)) for row in repair_release_rows),
            default=0,
        ),
        "repair_amplification": release_total / rho_peak if rho_peak else 0.0,
        "repair_overhead_ratio": (
            release_total / published_payload_bytes
            if published_payload_bytes else 0.0
        ),
        "observer_rho_max_bytes": max(
            (int(float(row.get("rho_bytes", "0") or 0)) for row in observer_rows),
            default=0,
        ),
        "observer_rho_final_bytes": observer_rho_final,
        "observer_nack_events_total": int(
            float(observer_last.get("nack_events_total", "0") or 0)
        ),
        "observer_nack_bytes_total": observer_nack_bytes,
        "observer_unique_repair_bytes_total": observer_unique_repair_bytes,
        "observer_repeated_nack_events_total": int(
            float(observer_last.get("repeated_nack_events_total", "0") or 0)
        ),
        "observer_repeated_nack_bytes_total": observer_repeated_nack_bytes,
        "observer_repeated_nack_ratio": observer_repeat_ratio,
        "observer_repair_release_events_total": int(
            float(observer_last.get("repair_release_events_total", "0") or 0)
        ),
        "observer_repair_release_bytes_total": observer_release_bytes,
        "observer_repair_amplification": observer_repair_amplification,
        "observer_repair_overhead_ratio": observer_repair_overhead,
        "feedback_amplification": feedback_amplification,
        "performance_collapse": performance_collapse,
        # A definitive storm also requires time-resolved on-wire retransmission data.
        "storm_candidate": storm_candidate,
        "observer_reader_guid": observer_reader,
        "control_count": len(control_rows),
        "repair_rate_min_mbps": min(rates, default=0.0),
        "repair_rate_max_mbps": max(rates, default=0.0),
        "path_rate_min_mbps": min(path_rates, default=0.0),
        "path_rate_max_mbps": max(path_rates, default=0.0),
        "decrease_count": decrease_count,
        "increase_count": increase_count,
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
    topic = f"calm{args.network}_{args.domain_base}_{run_number}_{scenario}"
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
    if args.shaping_enabled:
        apply_local_netem(args)
        apply_remote_netem(args, remote)
    snapshot_local(run_dir / "local_before.txt", args.interface)
    snapshot_remote(run_dir / "remote_before.txt", remote, args.remote_interface)

    common_remote_env = {
        "RMW_IMPLEMENTATION": "rmw_fastrtps_cpp",
        "ROS_LOCALHOST_ONLY": "0",
        "ROS_DOMAIN_ID": str(domain_id),
        "ROS_LOG_DIR": f"{remote_run_dir}/ros_log",
        "CALM_RESULT_DIR": remote_run_dir,
        "CALM_SUB_INACTIVE_TIMEOUT_S": str(args.sub_inactive_timeout_s),
        "FASTRTPS_DEFAULT_PROFILES_FILE": args.remote_profile,
        "CALM_NET_INTERFACE": args.remote_interface,
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

    background_subscribers: list[ManagedProcess] = []
    background_publishers: list[ManagedProcess] = []
    if scenario == "case_l":
        for flow in range(1, args.bg_flows + 1):
            bg_topic = f"{topic}_bg{flow}"
            bg_idx = run_number * 1000 + flow
            background_subscribers.append(
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
                        timeout_s=args.timeout_s + 10,
                    ),
                    os.environ.copy(),
                    run_dir / f"bg{flow}_sub.log",
                )
            )
        time.sleep(1.5)

    pub_environment = local_env(
        run_dir=run_dir,
        domain_id=domain_id,
        calm_env=calm_env,
        metrics=True,
        profile=args.local_profile,
        interface=args.interface,
        netem_rate_mbit=args.netem_rate_mbit if args.shaping_enabled else 0.0,
        netem_delay_ms=args.netem_delay_ms if args.shaping_enabled else 0.0,
        netem_jitter_ms=args.netem_jitter_ms if args.shaping_enabled else 0.0,
        netem_loss_pct=args.netem_loss_pct if args.shaping_enabled else 0.0,
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
                case_d_enabled=False,
            ),
        ],
        pub_environment,
        run_dir / "main_pub.log",
    )

    started = time.monotonic()
    disturbance_events = run_dir / "disturbance_events.csv"
    disturbance_done = scenario == "normal"
    disturbance_valid = scenario == "normal"
    while time.monotonic() - started < args.timeout_s:
        if not disturbance_done and pub.trigger.is_set():
            if scenario == "case_l":
                background_ready = True
                for cycle in range(1, args.case_l_cycles + 1):
                    append_disturbance_event(
                        disturbance_events,
                        "case_l",
                        cycle,
                        "background_launch",
                        (
                            f"flows={args.bg_flows} payload_kb={args.bg_payload_kb} "
                            f"hz={args.bg_hz}"
                        ),
                    )
                    bg_env = local_env(
                        run_dir=run_dir,
                        domain_id=domain_id,
                        calm_env=calm_env,
                        metrics=False,
                        profile=args.local_profile,
                        interface=args.interface,
                        netem_rate_mbit=args.netem_rate_mbit if args.shaping_enabled else 0.0,
                        netem_delay_ms=args.netem_delay_ms if args.shaping_enabled else 0.0,
                        netem_jitter_ms=args.netem_jitter_ms if args.shaping_enabled else 0.0,
                        netem_loss_pct=args.netem_loss_pct if args.shaping_enabled else 0.0,
                    )
                    for flow in range(1, args.bg_flows + 1):
                        bg_topic = f"{topic}_bg{flow}"
                        bg_idx = run_number * 100000 + cycle * 100 + flow
                        publisher = ManagedProcess(
                            [
                                str(
                                    Path(args.ws) /
                                    "install/calm_pretest_yw/lib/calm_pretest_yw/dds_pub"
                                ),
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
                            run_dir / f"bg{flow}_cycle{cycle}_pub.log",
                        )
                        background_publishers.append(publisher)

                    ready_deadline = time.monotonic() + 8.0
                    cycle_ready = all(
                        publisher.publish_started.wait(
                            max(0.0, ready_deadline - time.monotonic())
                        )
                        for publisher in background_publishers
                    )
                    background_ready &= cycle_ready
                    active_started = 0.0
                    if cycle_ready:
                        active_started = time.monotonic()
                        append_disturbance_event(
                            disturbance_events,
                            "case_l",
                            cycle,
                            "background_start",
                            (
                                f"flows={args.bg_flows} "
                                f"payload_kb={args.bg_payload_kb} "
                                f"hz={args.bg_hz}"
                            ),
                        )
                        time.sleep(args.case_duration_s)
                    else:
                        print(
                            f"[CALM-{args.network.upper()}] CASE L cycle {cycle} invalid: "
                            "background publisher discovery timeout",
                            flush=True,
                        )
                    for process in background_publishers:
                        process.terminate(grace_s=0.2)
                    background_publishers.clear()
                    append_disturbance_event(
                        disturbance_events,
                        "case_l",
                        cycle,
                        "background_end",
                        (
                            f"requested_s={args.case_duration_s:g} "
                            f"actual_s={max(0.0, time.monotonic() - active_started):.3f}"
                            if active_started else "publisher_discovery_failed"
                        ),
                    )
                    if cycle < args.case_l_cycles and args.case_l_recovery_s > 0:
                        append_disturbance_event(
                            disturbance_events,
                            "case_l",
                            cycle,
                            "recovery_window",
                            f"duration_s={args.case_l_recovery_s:g}",
                        )
                        time.sleep(args.case_l_recovery_s)

                for process in background_subscribers:
                    process.terminate()
                background_subscribers.clear()
                for flow in range(1, args.bg_flows + 1):
                    stop_remote_topic(remote, f"{topic}_bg{flow}")
                disturbance_valid = background_ready
                disturbance_done = True
            elif scenario == "case_d":
                disturbance_valid = run_case_d_cycles(
                    args,
                    remote,
                    disturbance_events,
                )
                disturbance_done = True
            else:
                disturbance_valid = True
                disturbance_done = True

        if pub.publish_done.is_set() and sub.wait(8.0) is not None:
            break
        if sub.poll() is not None:
            break
        time.sleep(0.1)

    for process in background_publishers:
        process.terminate()
    for process in background_subscribers:
        process.terminate()
    pub_exit = pub.terminate()
    sub_exit = sub.terminate()
    stop_remote_topic(remote, topic)
    elapsed = time.monotonic() - started

    copy_ok = copy_remote_results(remote, remote_run_dir, run_dir / "remote")
    snapshot_local(run_dir / "local_after.txt", args.interface)
    snapshot_remote(run_dir / "remote_after.txt", remote, args.remote_interface)
    row = summarize(
        run_dir,
        scenario,
        args.count,
        elapsed,
        args.payload_kb * 1024,
    )
    row.update(
        {
            "label": args.label,
            "experiment_mode": args.experiment_mode,
            "experiment_variant": args.experiment_variant,
            "calm_enabled": args.calm_enabled,
            "packetization": "mtu_safe" if args.mtu_safe else "stock",
            "rtps_message_size_limit": args.rtps_message_size_limit,
            "case_d_cycles": args.case_d_cycles,
            "case_d_recovery_s": args.case_d_recovery_s,
            "case_d_loss_pct": args.case_d_loss_pct,
            "case_d_bidirectional": args.case_d_bidirectional,
            "case_l_cycles": args.case_l_cycles,
            "case_l_recovery_s": args.case_l_recovery_s,
            "observer_enabled": args.observer_enabled,
            "run_number": run_number,
            "domain_id": domain_id,
            "pub_exit": pub_exit,
            "sub_exit": sub_exit,
            "disturbance_done": disturbance_valid,
            "remote_copy_ok": copy_ok,
            "run_dir": str(run_dir),
        }
    )
    return row


def scenario_duration(args: argparse.Namespace, scenario: str) -> float:
    if scenario == "case_l":
        return args.case_l_duration_s
    if scenario == "case_d":
        return args.case_d_duration_s
    return 0.0


def scenario_total_duration(args: argparse.Namespace, scenario: str) -> float:
    if scenario == "case_l":
        return (
            args.case_l_cycles * args.case_l_duration_s +
            max(0, args.case_l_cycles - 1) * args.case_l_recovery_s
        )
    if scenario == "case_d":
        return (
            args.case_d_cycles * args.case_d_duration_s +
            max(0, args.case_d_cycles - 1) * args.case_d_recovery_s
        )
    return 0.0


def evaluation_capacity_mbit(args: argparse.Namespace) -> float:
    if args.evaluation_capacity_mbit > 0:
        return args.evaluation_capacity_mbit
    if args.shaping_enabled and args.netem_rate_mbit > 0:
        return args.netem_rate_mbit
    return args.timeout_link_rate_mbit


def payload_load_mbps(payload_kb: int, hz: int) -> float:
    return payload_kb * 1024.0 * 8.0 * hz / 1_000_000.0


def build_sweep_plan(
        args: argparse.Namespace,
        scenarios: list[str],
        payloads: list[int],
        rates: list[int],
        experiment_modes: list[str],
) -> list[dict[str, object]]:
    plan: list[dict[str, object]] = []
    run_number = 0
    capacity_mbit = evaluation_capacity_mbit(args)
    for payload_kb in payloads:
        for hz in rates:
            offered_mbps = payload_load_mbps(payload_kb, hz)
            utilization = offered_mbps / capacity_mbit if capacity_mbit > 0 else math.inf
            in_scope = utilization <= args.max_baseline_utilization
            if args.in_scope_only and not in_scope:
                continue
            for scenario in scenarios:
                for experiment_mode in experiment_modes:
                    mode = EXPERIMENT_MODES[experiment_mode]
                    run_number += 1
                    rtps_message_size_limit = (
                        args.max_rtps_message_size if mode["mtu_safe"] else 0
                    )
                    run_key = (
                        f"{mode['variant'].lower()}_{experiment_mode}_"
                        f"p{payload_kb}_h{hz}_{scenario}"
                    )
                    plan.append(
                        {
                            "run_number": run_number,
                            "run_key": run_key,
                            "mode": experiment_mode,
                            "experiment_mode": experiment_mode,
                            "experiment_variant": mode["variant"],
                            "calm_enabled": mode["calm_enabled"],
                            "packetization": (
                                "mtu_safe" if mode["mtu_safe"] else "stock"
                            ),
                            "rtps_message_size_limit": rtps_message_size_limit,
                            "payload_kb": payload_kb,
                            "hz": hz,
                            "scenario": scenario,
                            "case_duration_s": scenario_duration(args, scenario),
                            "disturbance_total_s": scenario_total_duration(args, scenario),
                            "domain_id": args.domain_base + (run_number - 1) % args.domain_count,
                            "network": args.network,
                            "shaping": "on" if args.shaping_enabled else "off",
                            "payload_load_mbps": offered_mbps,
                            "evaluation_capacity_mbit": capacity_mbit,
                            "baseline_utilization": utilization,
                            "lambda_le_mu_scope": in_scope,
                        }
                    )
    return plan


def sweep_timeout(args: argparse.Namespace, spec: dict[str, object]) -> float:
    timeout = args.timeout_s
    if not args.auto_timeout:
        return timeout

    payload_kb = int(spec["payload_kb"])
    hz = int(spec["hz"])
    disturbance_s = float(spec["disturbance_total_s"])
    source_duration_s = args.count / hz
    wire_floor_s = source_duration_s
    if args.timeout_link_rate_mbit > 0:
        wire_floor_s = (
            payload_kb * 1024.0 * args.count * 8.0 /
            (args.timeout_link_rate_mbit * 1_000_000.0)
        )
    estimated = max(source_duration_s, wire_floor_s * args.timeout_factor)
    return max(timeout, math.ceil(estimated + disturbance_s + args.timeout_margin_s))


def build_single_run_command(
    args: argparse.Namespace,
    batch_dir: Path,
    spec: dict[str, object],
) -> list[str]:
    child_label = f"{args.label}_{int(spec['run_number']):03d}_{spec['run_key']}"
    timeout_s = sweep_timeout(args, spec)
    sub_timeout_s = max(args.sub_inactive_timeout_s, timeout_s)
    values = [
        ("--ws", args.ws),
        ("--network", args.network),
        ("--shaping", "on" if args.shaping_enabled else "off"),
        ("--local-ip", args.local_ip),
        ("--expected-ssid", args.expected_ssid),
        ("--result-root", str(batch_dir)),
        ("--remote-host", args.remote_host),
        ("--remote-management-host", args.remote_management_host),
        ("--remote-user", args.remote_user),
        ("--remote-ws", args.remote_ws),
        ("--remote-result-root", args.remote_result_root),
        ("--interface", args.interface),
        ("--remote-interface", args.remote_interface),
        ("--local-profile", args.local_profile),
        ("--remote-profile", args.remote_profile),
        ("--label", child_label),
        ("--scenarios", spec["scenario"]),
        ("--payload-kb", spec["payload_kb"]),
        ("--hz", spec["hz"]),
        ("--count", args.count),
        ("--max-samples", args.max_samples),
        ("--trigger-after", args.trigger_after),
        ("--case-duration-s", spec["case_duration_s"]),
        ("--case-d-cycles", args.case_d_cycles),
        ("--case-d-recovery-s", args.case_d_recovery_s),
        ("--case-d-loss-pct", args.case_d_loss_pct),
        ("--case-d-bidirectional", int(args.case_d_bidirectional)),
        ("--case-l-cycles", args.case_l_cycles),
        ("--case-l-recovery-s", args.case_l_recovery_s),
        ("--observer-enabled", int(args.observer_enabled)),
        ("--bg-payload-kb", args.bg_payload_kb),
        ("--bg-hz", args.bg_hz),
        ("--bg-flows", args.bg_flows),
        ("--timeout-s", timeout_s),
        ("--sub-inactive-timeout-s", sub_timeout_s),
        ("--timeout-link-rate-mbit", args.timeout_link_rate_mbit),
        ("--domain-base", spec["domain_id"]),
        ("--controller", args.controller),
        ("--experiment-mode", spec["experiment_mode"]),
        ("--gamma", args.gamma),
        ("--alpha", args.alpha),
        ("--q-min", args.q_min),
        ("--pacing-ms", args.pacing_ms),
        ("--fragment-bytes-per-period", args.fragment_bytes_per_period),
        ("--fragment-period-ms", args.fragment_period_ms),
        ("--max-rtps-message-size", args.max_rtps_message_size),
        ("--max-budget-bytes", args.max_budget_bytes),
        ("--max-scheduled-bytes", args.max_scheduled_bytes),
        ("--budget-horizon-ms", args.budget_horizon_ms),
        ("--delta-threshold-bytes", args.delta_threshold_bytes),
        ("--onset-threshold-bytes", args.onset_threshold_bytes),
        ("--retry-cooldown-ms", args.retry_cooldown_ms),
        ("--first-repair-delay-ms", args.first_repair_delay_ms),
        ("--post-repair-guard-ms", args.post_repair_guard_ms),
        ("--recovery-probe-delay-ms", args.recovery_probe_delay_ms),
        ("--min-repair-rate-mbps", args.min_repair_rate_mbps),
        ("--initial-repair-rate-mbps", args.initial_repair_rate_mbps),
        ("--max-repair-rate-mbps", args.max_repair_rate_mbps),
        ("--max-path-rate-mbps", args.max_path_rate_mbps),
        ("--min-path-rate-mbps", args.min_path_rate_mbps),
        ("--path-gamma", args.path_gamma),
        ("--path-alpha", args.path_alpha),
        ("--hold-mode", args.hold_mode),
        ("--kp", args.kp),
        ("--ki", args.ki),
        ("--kd", args.kd),
        ("--netem-rate-mbit", args.netem_rate_mbit),
        ("--netem-delay-ms", args.netem_delay_ms),
        ("--netem-jitter-ms", args.netem_jitter_ms),
        ("--netem-loss-pct", args.netem_loss_pct),
        ("--netem-limit-packets", args.netem_limit_packets),
    ]
    command = [sys.executable, str(Path(__file__).resolve())]
    for option, value in values:
        command.extend((option, str(value)))
    return command


def rebuild_sweep_summary(
    batch_dir: Path,
    plan: list[dict[str, object]],
    progress: list[dict[str, object]],
) -> None:
    latest_completed: dict[str, dict[str, object]] = {}
    for entry in progress:
        if entry.get("status") == "completed":
            latest_completed[str(entry["run_key"])] = entry

    rows: list[dict[str, object]] = []
    metadata_fields = [
        "sweep_run_number",
        "run_key",
        "mode",
        "experiment_mode",
        "experiment_variant",
        "calm_enabled",
        "packetization",
        "rtps_message_size_limit",
        "payload_kb",
        "hz",
        "sweep_domain_id",
        "network",
        "shaping",
        "payload_load_mbps",
        "evaluation_capacity_mbit",
        "baseline_utilization",
        "lambda_le_mu_scope",
        "disturbance_total_s",
    ]
    for spec in plan:
        entry = latest_completed.get(str(spec["run_key"]))
        if entry is None:
            continue
        result_dir = Path(str(entry["result_dir"]))
        child_rows = read_csv(result_dir / "summary.csv")
        if not child_rows:
            continue
        child_row = child_rows[0]
        row: dict[str, object] = {
            "sweep_run_number": spec["run_number"],
            "run_key": spec["run_key"],
            "mode": spec["mode"],
            "experiment_mode": spec["experiment_mode"],
            "experiment_variant": spec["experiment_variant"],
            "calm_enabled": spec["calm_enabled"],
            "packetization": spec["packetization"],
            "rtps_message_size_limit": spec["rtps_message_size_limit"],
            "payload_kb": spec["payload_kb"],
            "hz": spec["hz"],
            "sweep_domain_id": spec["domain_id"],
            "network": spec["network"],
            "shaping": spec["shaping"],
            "payload_load_mbps": spec["payload_load_mbps"],
            "evaluation_capacity_mbit": spec["evaluation_capacity_mbit"],
            "baseline_utilization": spec["baseline_utilization"],
            "lambda_le_mu_scope": spec["lambda_le_mu_scope"],
            "disturbance_total_s": spec["disturbance_total_s"],
        }
        row.update(child_row)
        rows.append(row)

    if not rows:
        return
    child_fields = [field for field in rows[0] if field not in metadata_fields]
    with (batch_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=metadata_fields + child_fields)
        writer.writeheader()
        writer.writerows(rows)


def write_sweep_plan(batch_dir: Path, plan: list[dict[str, object]]) -> None:
    atomic_write_json(batch_dir / "sweep_plan.json", plan)
    with (batch_dir / "sweep_plan.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(plan[0]))
        writer.writeheader()
        writer.writerows(plan)


def create_resume_script(args: argparse.Namespace, batch_dir: Path) -> None:
    command = [
        sys.executable,
        str(Path(__file__).resolve()),
        *sys.argv[1:],
        "--resume-dir",
        str(batch_dir),
    ]
    script = batch_dir / "resume_sweep.sh"
    script.write_text(
        "#!/usr/bin/env bash\n"
        "set -e\n"
        "source /opt/ros/humble/setup.bash\n"
        f"source {shlex.quote(args.ws)}/install/setup.bash\n"
        f"exec {shlex.join(command)}\n",
        encoding="utf-8",
    )
    script.chmod(0o755)


def run_sweep(
        args: argparse.Namespace,
        scenarios: list[str],
        payloads: list[int],
        rates: list[int],
        experiment_modes: list[str],
) -> int:
    plan = build_sweep_plan(args, scenarios, payloads, rates, experiment_modes)
    total_timeout_s = sum(sweep_timeout(args, spec) for spec in plan)
    print(
        f"[CALM-SWEEP] planned runs={len(plan)} "
        f"network={args.network} shaping={'on' if args.shaping_enabled else 'off'} "
        f"maximum timeout sum={total_timeout_s / 3600.0:.1f}h",
        flush=True,
    )
    for spec in plan:
        print(
            f"[CALM-SWEEP] {int(spec['run_number']):03d}/{len(plan)} "
            f"{spec['run_key']} domain={spec['domain_id']} "
            f"lambda={float(spec['payload_load_mbps']):.1f}Mbps "
            f"util={float(spec['baseline_utilization']):.2f} "
            f"scope={'yes' if spec['lambda_le_mu_scope'] else 'overload'} "
            f"timeout={sweep_timeout(args, spec):.0f}s",
            flush=True,
        )
    if args.dry_run:
        return 0

    if args.resume_dir:
        batch_dir = Path(args.resume_dir).expanduser().resolve()
        if not batch_dir.is_dir():
            raise ValueError(f"resume directory does not exist: {batch_dir}")
        plan_path = batch_dir / "sweep_plan.json"
        if not plan_path.is_file():
            raise ValueError(f"sweep plan not found: {plan_path}")
        with plan_path.open(encoding="utf-8") as stream:
            saved_plan = json.load(stream)
        if saved_plan != plan:
            raise ValueError(
                "resume arguments do not match the saved sweep plan; "
                "use the generated resume_sweep.sh"
            )
    else:
        batch_dir = (
            Path(args.result_root).resolve() /
            f"CALM3_{args.network}_sweep_{args.label}_{timestamp()}"
        )
        batch_dir.mkdir(parents=True, exist_ok=False)
        write_sweep_plan(batch_dir, plan)
        atomic_write_json(batch_dir / "sweep_parameters.json", vars(args))
        create_resume_script(args, batch_dir)

    progress_path = batch_dir / "sweep_progress.json"
    if progress_path.is_file():
        with progress_path.open(encoding="utf-8") as stream:
            progress: list[dict[str, object]] = json.load(stream)
    else:
        progress = []
    completed = {
        str(entry["run_key"])
        for entry in progress
        if entry.get("status") == "completed"
    }
    runner_log = batch_dir / "sweep_runner.log"
    failed_this_pass = 0
    consecutive_failures = 0

    for spec in plan:
        run_key = str(spec["run_key"])
        run_number = int(spec["run_number"])
        if run_key in completed:
            print(
                f"[CALM-SWEEP] skip completed {run_number:03d}/{len(plan)} {run_key}",
                flush=True,
            )
            continue

        command = build_single_run_command(args, batch_dir, spec)
        child_label = f"{args.label}_{run_number:03d}_{run_key}"
        print(
            f"[CALM-SWEEP] start {run_number:03d}/{len(plan)} {run_key}",
            flush=True,
        )
        started_at = dt.datetime.now().isoformat()
        started = time.monotonic()
        with runner_log.open("a", encoding="utf-8") as log:
            log.write(f"\n[{started_at}] $ {shlex.join(command)}\n")
            log.flush()
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            assert process.stdout is not None
            try:
                for line in process.stdout:
                    print(line, end="", flush=True)
                    log.write(line)
                    log.flush()
                return_code = process.wait()
            except KeyboardInterrupt:
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=10.0)
                except subprocess.TimeoutExpired:
                    process.terminate()
                raise

        candidates = sorted(
            batch_dir.glob(f"CALM3_{args.network}_{child_label}_*"),
            key=lambda path: path.stat().st_mtime,
        )
        result_dir = candidates[-1] if candidates else None
        child_summary = (
            read_csv(result_dir / "summary.csv")
            if result_dir is not None
            else []
        )
        summary_ok = bool(child_summary)
        if summary_ok:
            result_row = child_summary[0]
            summary_ok = (
                result_row.get("remote_copy_ok", "").lower() == "true" and
                result_row.get("disturbance_done", "").lower() == "true"
            )
        status = "completed" if return_code == 0 and summary_ok else "failed"
        if status == "failed":
            failed_this_pass += 1
            consecutive_failures += 1
        else:
            consecutive_failures = 0
        entry: dict[str, object] = {
            **spec,
            "attempt": sum(
                previous.get("run_key") == run_key for previous in progress
            ) + 1,
            "status": status,
            "return_code": return_code,
            "started_at": started_at,
            "elapsed_s": time.monotonic() - started,
            "result_dir": str(result_dir) if result_dir else "",
        }
        progress.append(entry)
        atomic_write_json(progress_path, progress)
        rebuild_sweep_summary(batch_dir, plan, progress)
        print(
            f"[CALM-SWEEP] {status} {run_number:03d}/{len(plan)} {run_key} "
            f"elapsed={entry['elapsed_s']:.1f}s",
            flush=True,
        )
        if args.inter_run_delay_s > 0:
            time.sleep(args.inter_run_delay_s)
        if consecutive_failures >= args.max_consecutive_failures:
            print(
                f"[CALM-SWEEP] stopping after {consecutive_failures} consecutive "
                "failures; restore the link and run resume_sweep.sh",
                flush=True,
            )
            break

    completed_after = {
        str(entry["run_key"])
        for entry in progress
        if entry.get("status") == "completed"
    }
    print(
        f"[CALM-SWEEP] completed={len(completed_after)}/{len(plan)} "
        f"results={batch_dir}",
        flush=True,
    )
    print(f"[CALM-SWEEP] resume={batch_dir / 'resume_sweep.sh'}", flush=True)
    return 1 if failed_this_pass else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ws", default="/home/csi/ros2_ws")
    parser.add_argument(
        "--network",
        choices=tuple(NETWORK_PRESETS),
        default="ethernet",
        help="Select interface, IP, and DDS profile defaults.",
    )
    parser.add_argument(
        "--shaping",
        choices=("auto", "on", "off"),
        default="auto",
        help="auto enables baseline netem only for Ethernet; Wi-Fi stays physical.",
    )
    parser.add_argument("--local-ip", default=None)
    parser.add_argument("--expected-ssid", default=None)
    parser.add_argument("--result-root", default="/home/csi/ros2_ws/results/test_yw_2")
    parser.add_argument("--remote-host", default=None)
    parser.add_argument(
        "--remote-management-host",
        default=None,
        help=(
            "Optional SSH management address. Use the Wi-Fi address here when "
            "CASE D shapes both Ethernet directions, so recovery commands do "
            "not traverse the impaired DDS link."
        ),
    )
    parser.add_argument("--remote-user", default="csilab")
    parser.add_argument("--remote-ws", default="/home/csilab/ros2_ws")
    parser.add_argument("--remote-result-root", default="/home/csilab/ros2_ws/results/test_yw_2")
    parser.add_argument("--interface", default=None)
    parser.add_argument("--remote-interface", default=None)
    parser.add_argument("--local-profile", default=None)
    parser.add_argument("--remote-profile", default=None)
    parser.add_argument("--label", default="candidate")
    parser.add_argument("--scenarios", default="normal,case_d,case_l")
    parser.add_argument(
        "--sweep",
        action="store_true",
        help="Run the payload/Hz/scenario/A-B-C Cartesian product sequentially.",
    )
    parser.add_argument(
        "--payload-kb-list",
        default="8192,4096,2048,1024,512,256,128",
    )
    parser.add_argument("--hz-list", default="10,20,30")
    parser.add_argument(
        "--experiment-modes",
        default="stock_default,mtu_default,mtu_calm",
        help=(
            "Comma-separated A/B/C modes: stock_default, mtu_default, mtu_calm. "
            "They run in the given order for every payload/Hz/scenario."
        ),
    )
    parser.add_argument(
        "--calm-modes",
        default="",
        help=(
            "Deprecated compatibility option: 0 maps to stock_default and "
            "1 maps to mtu_calm. Prefer --experiment-modes."
        ),
    )
    parser.add_argument(
        "--resume-dir",
        default="",
        help="Resume an interrupted sweep from its top-level result directory.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the sweep plan and timeout estimate without running experiments.",
    )
    parser.add_argument("--payload-kb", type=int, default=1024)
    parser.add_argument("--hz", type=int, default=10)
    parser.add_argument("--count", type=int, default=240)
    parser.add_argument("--max-samples", type=int, default=400)
    parser.add_argument("--trigger-after", type=int, default=60)
    parser.add_argument("--case-duration-s", type=float, default=3.0)
    parser.add_argument("--case-l-duration-s", type=float, default=5.0)
    parser.add_argument("--case-d-duration-s", type=float, default=20.0)
    parser.add_argument("--case-d-cycles", type=int, default=1)
    parser.add_argument("--case-d-recovery-s", type=float, default=5.0)
    parser.add_argument("--case-d-loss-pct", type=float, default=100.0)
    parser.add_argument("--case-d-bidirectional", type=int, choices=(0, 1), default=0)
    parser.add_argument("--case-l-cycles", type=int, default=1)
    parser.add_argument("--case-l-recovery-s", type=float, default=5.0)
    parser.add_argument("--observer-enabled", type=int, choices=(0, 1), default=1)
    parser.add_argument("--bg-payload-kb", type=int, default=1024)
    parser.add_argument("--bg-hz", type=int, default=20)
    parser.add_argument("--bg-flows", type=int, default=2)
    parser.add_argument("--timeout-s", type=float, default=90.0)
    parser.add_argument("--sub-inactive-timeout-s", type=float, default=120.0)
    parser.add_argument(
        "--auto-timeout",
        action="store_true",
        help="Increase each timeout from payload size and the shaped-link wire-time floor.",
    )
    parser.add_argument("--timeout-factor", type=float, default=1.35)
    parser.add_argument("--timeout-margin-s", type=float, default=60.0)
    parser.add_argument(
        "--timeout-link-rate-mbit",
        type=float,
        default=120.0,
        help="Capacity estimate used only by auto-timeout, not Wi-Fi shaping.",
    )
    parser.add_argument(
        "--evaluation-capacity-mbit",
        type=float,
        default=0.0,
        help="DDS payload capacity used to classify lambda/mu; 0 uses netem or timeout capacity.",
    )
    parser.add_argument(
        "--max-baseline-utilization",
        type=float,
        default=0.80,
        help="Maximum baseline lambda/mu classified as an in-scope CALM run.",
    )
    parser.add_argument(
        "--in-scope-only",
        action="store_true",
        help="Skip payload/Hz combinations above max-baseline-utilization.",
    )
    parser.add_argument("--inter-run-delay-s", type=float, default=2.0)
    parser.add_argument("--max-consecutive-failures", type=int, default=3)
    parser.add_argument("--domain-base", type=int, default=200)
    parser.add_argument(
        "--domain-count",
        type=int,
        default=16,
        help="Number of ROS domain IDs rotated by sweep mode.",
    )
    parser.add_argument("--controller", choices=("aimd", "pi", "pd", "pid"), default="pi")
    parser.add_argument(
        "--experiment-mode",
        choices=tuple(EXPERIMENT_MODES),
        default="",
        help="Single-run A/B/C experiment mode; sweep mode sets this automatically.",
    )
    parser.add_argument("--calm-enabled", type=int, choices=(0, 1), default=1)
    parser.add_argument("--gamma", type=float, default=0.65)
    parser.add_argument("--alpha", type=float, default=64.0)
    parser.add_argument("--q-min", type=float, default=0.125)
    parser.add_argument("--pacing-ms", type=float, default=1.0)
    parser.add_argument("--fragment-bytes-per-period", type=int, default=32 * 1024)
    parser.add_argument("--fragment-period-ms", type=int, default=1)
    parser.add_argument("--max-rtps-message-size", type=int, default=1400)
    parser.add_argument("--max-budget-bytes", type=int, default=512 * 1024)
    parser.add_argument("--max-scheduled-bytes", type=int, default=1 * 1024 * 1024)
    parser.add_argument("--budget-horizon-ms", type=float, default=20.0)
    parser.add_argument("--delta-threshold-bytes", type=int, default=16 * 1024)
    parser.add_argument("--onset-threshold-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--retry-cooldown-ms", type=float, default=100.0)
    parser.add_argument("--first-repair-delay-ms", type=float, default=25.0)
    parser.add_argument("--post-repair-guard-ms", type=float, default=100.0)
    parser.add_argument("--recovery-probe-delay-ms", type=float, default=150.0)
    parser.add_argument("--min-repair-rate-mbps", type=float, default=16.0)
    parser.add_argument("--initial-repair-rate-mbps", type=float, default=96.0)
    parser.add_argument("--max-repair-rate-mbps", type=float, default=192.0)
    parser.add_argument(
        "--max-path-rate-mbps",
        "--held-new-rate-mbps",
        dest="max_path_rate_mbps",
        type=float,
        default=300.0,
    )
    parser.add_argument("--min-path-rate-mbps", type=float, default=16.0)
    parser.add_argument("--path-gamma", type=float, default=0.75)
    parser.add_argument("--path-alpha", type=float, default=64.0)
    parser.add_argument("--hold-mode", type=int, choices=(0, 1, 2), default=1)
    parser.add_argument("--kp", type=float, default=0.30)
    parser.add_argument("--ki", type=float, default=0.08)
    parser.add_argument("--kd", type=float, default=0.75)
    parser.add_argument("--netem-rate-mbit", type=float, default=120.0)
    parser.add_argument("--netem-delay-ms", type=float, default=3.0)
    parser.add_argument("--netem-jitter-ms", type=float, default=0.0)
    parser.add_argument("--netem-loss-pct", type=float, default=0.0)
    parser.add_argument("--netem-limit-packets", type=int, default=10000)
    args = parser.parse_args()
    resolve_network_settings(args)
    args.case_d_bidirectional = bool(args.case_d_bidirectional)
    args.observer_enabled = bool(args.observer_enabled)
    if args.max_rtps_message_size < 512 or args.max_rtps_message_size > 65500:
        parser.error("max-rtps-message-size must be within 512..65500 bytes")
    if args.case_d_cycles <= 0 or args.case_l_cycles <= 0:
        parser.error("case-d-cycles and case-l-cycles must be positive")
    if args.case_d_recovery_s < 0 or args.case_l_recovery_s < 0:
        parser.error("CASE D/L recovery intervals cannot be negative")
    if not 0.0 <= args.case_d_loss_pct <= 100.0:
        parser.error("case-d-loss-pct must be within 0..100")
    if (
        args.case_d_bidirectional and
        args.remote_management_host == args.remote_host
    ):
        parser.error(
            "bidirectional CASE D requires --remote-management-host on a "
            "separate management path; otherwise the script cannot restore "
            "the remote qdisc"
        )

    scenarios = [item.strip() for item in args.scenarios.split(",") if item.strip()]
    invalid = sorted(set(scenarios) - set(SCENARIOS))
    if invalid:
        parser.error(f"unsupported scenarios: {', '.join(invalid)}")
    if args.sweep or args.resume_dir:
        try:
            payloads = parse_integer_list(args.payload_kb_list, "payload-kb-list")
            rates = parse_integer_list(args.hz_list, "hz-list")
            if args.calm_modes:
                calm_modes = parse_integer_list(args.calm_modes, "calm-modes", {0, 1})
                experiment_modes = [
                    "stock_default" if mode == 0 else "mtu_calm"
                    for mode in calm_modes
                ]
            else:
                experiment_modes = parse_choice_list(
                    args.experiment_modes,
                    "experiment-modes",
                    set(EXPERIMENT_MODES),
                )
        except ValueError as error:
            parser.error(str(error))
        if args.domain_count <= 0:
            parser.error("domain-count must be positive")
        if args.domain_base < 0 or args.domain_base + args.domain_count - 1 > 232:
            parser.error("sweep ROS domain ID pool must be within 0..232")
        if args.count <= 0:
            parser.error("count must be positive")
        if args.trigger_after < 0 or args.trigger_after >= args.count:
            parser.error("trigger-after must be within 0..count-1")
        if args.timeout_factor < 1.0:
            parser.error("timeout-factor must be at least 1.0")
        if args.max_baseline_utilization <= 0:
            parser.error("max-baseline-utilization must be positive")
        if args.in_scope_only and evaluation_capacity_mbit(args) <= 0:
            parser.error("in-scope-only requires a positive evaluation capacity")
        if args.max_consecutive_failures <= 0:
            parser.error("max-consecutive-failures must be positive")
        try:
            if not args.dry_run:
                preflight_environment(args, scenarios)
            return run_sweep(args, scenarios, payloads, rates, experiment_modes)
        except ValueError as error:
            parser.error(str(error))
    apply_experiment_mode(args)
    if args.domain_base < 0 or args.domain_base + len(scenarios) - 1 > 232:
        parser.error("ROS domain IDs must be within 0..232")

    try:
        remote = preflight_environment(args, scenarios)
    except ValueError as error:
        parser.error(str(error))

    batch_dir = Path(args.result_root).resolve() / (
        f"CALM3_{args.network}_{args.label}_{timestamp()}"
    )
    batch_dir.mkdir(parents=True, exist_ok=True)
    with (batch_dir / "parameters.json").open("w", encoding="utf-8") as stream:
        json.dump(vars(args), stream, indent=2, sort_keys=True)
        stream.write("\n")

    calm_env = {
        "FASTDDS_CALM_ENABLED": str(args.calm_enabled),
        "FASTDDS_CALM_OBSERVER_ENABLED": str(int(args.observer_enabled)),
        "FASTDDS_CALM_GAMMA": str(args.gamma),
        "FASTDDS_CALM_ALPHA": str(args.alpha),
        "FASTDDS_CALM_Q_MIN": str(args.q_min),
        "FASTDDS_CALM_CONTROLLER": args.controller,
        "FASTDDS_CALM_PACING_MS": str(args.pacing_ms),
        "FASTDDS_CALM_MAX_BUDGET_BYTES": str(args.max_budget_bytes),
        "FASTDDS_CALM_MAX_SCHEDULED_BYTES": str(args.max_scheduled_bytes),
        "FASTDDS_CALM_BUDGET_HORIZON_MS": str(args.budget_horizon_ms),
        "FASTDDS_CALM_DELTA_THRESHOLD_BYTES": str(args.delta_threshold_bytes),
        "FASTDDS_CALM_ONSET_THRESHOLD_BYTES": str(args.onset_threshold_bytes),
        "FASTDDS_CALM_RETRY_COOLDOWN_MS": str(args.retry_cooldown_ms),
        "FASTDDS_CALM_FIRST_REPAIR_DELAY_MS": str(args.first_repair_delay_ms),
        "FASTDDS_CALM_POST_REPAIR_GUARD_MS": str(args.post_repair_guard_ms),
        "FASTDDS_CALM_RECOVERY_PROBE_DELAY_MS": str(args.recovery_probe_delay_ms),
        "FASTDDS_CALM_MIN_REPAIR_RATE_MBPS": str(args.min_repair_rate_mbps),
        "FASTDDS_CALM_INITIAL_REPAIR_RATE_MBPS": str(args.initial_repair_rate_mbps),
        "FASTDDS_CALM_MAX_REPAIR_RATE_MBPS": str(args.max_repair_rate_mbps),
        "FASTDDS_CALM_MAX_PATH_RATE_MBPS": str(args.max_path_rate_mbps),
        "FASTDDS_CALM_MIN_PATH_RATE_MBPS": str(args.min_path_rate_mbps),
        "FASTDDS_CALM_PATH_GAMMA": str(args.path_gamma),
        "FASTDDS_CALM_PATH_ALPHA": str(args.path_alpha),
        "FASTDDS_CALM_HOLD_MODE": str(args.hold_mode),
        "FASTDDS_CALM_KP": str(args.kp),
        "FASTDDS_CALM_KI": str(args.ki),
        "FASTDDS_CALM_KD": str(args.kd),
        "FASTDDS_CALM_FRAGMENT_BYTES_PER_PERIOD": str(args.fragment_bytes_per_period),
        "FASTDDS_CALM_FRAGMENT_PERIOD_MS": str(args.fragment_period_ms),
        "FASTDDS_CALM_MAX_RTPS_MESSAGE_SIZE": str(args.max_rtps_message_size),
        "FASTDDS_RTPS_MAX_MESSAGE_SIZE": str(args.rtps_message_size_limit),
    }

    rows: list[dict[str, object]] = []
    try:
        for index, scenario in enumerate(scenarios, start=1):
            print(
                f"[CALM-{args.network.upper()}] {args.label}: {scenario}",
                flush=True,
            )
            row = run_scenario(
                args=args,
                batch_dir=batch_dir,
                calm_env=calm_env,
                scenario=scenario,
                run_number=index,
            )
            rows.append(row)
            print(
                f"[CALM-{args.network.upper()}] {scenario}: "
                f"recv={row['received_count']}/{row['requested_count']} "
                f"p95={row['sub_delay_p95_ms']:.1f}ms rho={row['rho_max_bytes']} "
                f"repair={row['repair_release_total_bytes']}",
                flush=True,
            )
    finally:
        if args.shaping_enabled:
            clear_local_netem(args.interface)
            clear_remote_netem(remote, args.remote_interface)
        elif "case_d" in scenarios:
            clear_local_netem(args.interface)

    if rows:
        with (batch_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    print(f"[CALM-{args.network.upper()}] results: {batch_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
