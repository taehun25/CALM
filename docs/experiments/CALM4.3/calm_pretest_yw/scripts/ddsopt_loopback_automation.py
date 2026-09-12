#!/usr/bin/env python3
"""Compare OPT1/OPT1+2 over UDP loopback with CALM on or off."""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import signal
import statistics
import subprocess
import threading
import time
from dataclasses import asdict
from pathlib import Path

from calm_loopback_optimize import (
    ManagedProcess,
    main_pub_args,
    percentile,
    read_csv,
    ros_command,
    sub_args,
    timestamp,
)
from dds_wireless_optimizer import (
    compute_parameters,
    generate_cyclonedds_profiles,
    generate_opt1_profiles,
    generate_profiles,
)


MODES = ("opt1", "opt12")
CONTROLS = ("default", "calm")
SCENARIOS = ("normal", "case_l", "case_d", "case_a")


def comma_list(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def comma_ints(value: str) -> list[int]:
    try:
        values = [int(item) for item in comma_list(value)]
    except ValueError as error:
        raise SystemExit(f"invalid integer list: {value}") from error
    if any(item <= 0 for item in values):
        raise SystemExit(f"all list values must be positive: {value}")
    return values


def comma_floats(value: str) -> list[float]:
    try:
        return [float(item) for item in comma_list(value)]
    except ValueError as error:
        raise SystemExit(f"invalid floating-point list: {value}") from error


def privileged_command(*command: str) -> list[str]:
    if os.geteuid() == 0:
        return list(command)
    return ["sudo", "-n", *command]


def configure_qdisc(
        *,
        rate_mbit: int,
        delay_ms: float,
        jitter_ms: float,
        loss_percent: float,
        limit: int) -> None:
    command = privileged_command(
        "tc", "qdisc", "replace", "dev", "lo", "root", "netem")
    if limit > 0:
        command.extend(["limit", str(limit)])
    command.extend(["rate", f"{rate_mbit}mbit"])
    if delay_ms > 0 or jitter_ms > 0:
        command.extend(["delay", f"{delay_ms:g}ms", f"{jitter_ms:g}ms"])
    command.extend(["loss", f"{loss_percent:g}%"])
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"failed to configure loopback qdisc: {detail}")


def remove_qdisc() -> None:
    subprocess.run(
        privileged_command("tc", "qdisc", "del", "dev", "lo", "root"),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def keep_sudo_ticket(stop: threading.Event) -> None:
    while not stop.wait(60.0):
        subprocess.run(
            ["sudo", "-n", "-v"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


def process_env(
        *,
        run_dir: Path,
        domain_id: int,
        profile: Path,
        storm_log: Path | None,
        calm_enabled: bool,
        args: argparse.Namespace) -> dict[str, str]:
    env = os.environ.copy()
    env["RMW_IMPLEMENTATION"] = "rmw_fastrtps_cpp"
    # The XML pins UDP to 127.0.0.1 and disables Data Sharing. Keeping this at
    # zero avoids Humble's localhost-only path adding an SHM transport.
    env["ROS_LOCALHOST_ONLY"] = "0"
    env["ROS_DOMAIN_ID"] = str(domain_id)
    env["ROS_LOG_DIR"] = str(run_dir / "ros_log")
    env["CALM_RESULT_DIR"] = str(run_dir)
    if args.dds == "cyclonedds":
        env["RMW_IMPLEMENTATION"] = "rmw_cyclonedds_cpp"
        env["CYCLONEDDS_URI"] = f"file://{profile}"
        env.pop("FASTRTPS_DEFAULT_PROFILES_FILE", None)
        env.pop("FASTDDS_DEFAULT_PROFILES_FILE", None)
        env.pop("RMW_FASTRTPS_USE_QOS_FROM_XML", None)
        for name in tuple(env):
            if name.startswith("CYCLONEDDS_CALM_"):
                env.pop(name, None)
        env["CYCLONEDDS_CALM_ENABLED"] = "1" if calm_enabled else "0"
        if calm_enabled:
            env["CYCLONEDDS_CALM_CONTROLLER"] = args.calm_controller
            env["CYCLONEDDS_CALM_PACING_MS"] = str(
                args.calm_pacing_ms)
            env["CYCLONEDDS_CALM_HEARTBEAT_MS"] = str(500.0 / args.hz)
            env["CYCLONEDDS_CALM41_TIMEOUT_RTT_MULTIPLIER"] = str(
                args.calm41_timeout_rtt_multiplier)
            env["CYCLONEDDS_CALM_FEEDBACK_RTT_ALPHA"] = str(
                args.calm_feedback_rtt_alpha)
            env["CYCLONEDDS_CALM4_K_DEC"] = str(args.calm4_k_dec)
            env["CYCLONEDDS_CALM4_K_INC"] = str(args.calm4_k_inc)
            env["CYCLONEDDS_CALM_PACING_DRAIN_GUARD"] = str(
                args.calm_pacing_drain_guard)
            env["CYCLONEDDS_CALM_MIN_BUDGET_BYTES"] = str(
                args.calm_min_budget_bytes)
            env["CYCLONEDDS_CALM_INITIAL_BUDGET_BYTES"] = str(
                args.calm_initial_budget_bytes)
            env["CYCLONEDDS_CALM_MAX_BUDGET_BYTES"] = str(
                args.calm_max_budget_bytes)
            env["CYCLONEDDS_CALM_AI_B_BYTES"] = str(args.calm_ai_bytes)
            env["CYCLONEDDS_CALM_MIN_BUDGET_SAMPLE_MULTIPLIER"] = str(
                args.calm_min_budget_sample_multiplier)
            env["CYCLONEDDS_CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER"] = str(
                args.calm_initial_budget_sample_multiplier)
            env["CYCLONEDDS_CALM_MAX_BUDGET_SAMPLE_MULTIPLIER"] = str(
                args.calm_max_budget_sample_multiplier)
            env["CYCLONEDDS_CALM_AI_B_SAMPLE_MULTIPLIER"] = str(
                args.calm_ai_b_sample_multiplier)
            env["CYCLONEDDS_CALM_FIRST_FAILURE_GAMMA"] = str(
                args.calm_first_failure_gamma)
            env["CYCLONEDDS_CALM_REPEATED_FAILURE_GAMMA"] = str(
                args.calm_gamma)
            env["CYCLONEDDS_CALM_FIRST_FAILURE_RATE_GAMMA"] = str(
                args.calm_first_failure_rate_gamma)
            env["CYCLONEDDS_CALM_REPEATED_FAILURE_RATE_GAMMA"] = str(
                args.calm_rate_gamma)
            env["CYCLONEDDS_CALM_RHO_SAMPLE_MULTIPLIER"] = str(
                args.calm_rho_sample_multiplier)
            env["CYCLONEDDS_CALM_GROWTH_ROUNDS"] = str(
                args.calm_growth_rounds)
            env["CYCLONEDDS_CALM_ACK_STALL_MS"] = str(
                args.calm_ack_stall_ms)
            env["CYCLONEDDS_CALM_FEEDBACK_GUARD_MS"] = str(
                args.cyclonedds_calm_feedback_min_age_ms)
            env["CYCLONEDDS_CALM_OFFERED_RATE_EWMA_ALPHA"] = str(
                args.calm_offered_rate_ewma_alpha)
            env["CYCLONEDDS_CALM_SERVICE_RATE_MIN_MULTIPLIER"] = str(
                args.calm_service_rate_min_multiplier)
            env["CYCLONEDDS_CALM_SERVICE_RATE_INITIAL_MULTIPLIER"] = str(
                args.calm_service_rate_initial_multiplier)
            env["CYCLONEDDS_CALM_SERVICE_RATE_MAX_MULTIPLIER"] = str(
                args.calm_service_rate_max_multiplier)
            env["CYCLONEDDS_CALM_SERVICE_RATE_AI_MULTIPLIER"] = str(
                args.calm_service_rate_ai_multiplier)
            env["CYCLONEDDS_CALM_SERVICE_RATE_ALPHA_UP"] = str(
                args.calm_service_rate_alpha_up)
            env["CYCLONEDDS_CALM_SERVICE_RATE_ALPHA_DOWN"] = str(
                args.calm_service_rate_alpha_down)
            env["CYCLONEDDS_CALM_SERVICE_RATE_FAILURE_GAMMA"] = str(
                args.calm_service_rate_failure_gamma)
            env["CYCLONEDDS_CALM_FEEDBACK_BYTE_PROGRESS"] = (
                "1" if args.calm_feedback_byte_progress else "0")
        if storm_log is None:
            env["CYCLONEDDS_STORM_OBSERVER_ENABLED"] = "0"
            env.pop("CYCLONEDDS_STORM_LOG_FILE", None)
        else:
            env["CYCLONEDDS_STORM_OBSERVER_ENABLED"] = "1"
            env["CYCLONEDDS_STORM_LOG_FILE"] = str(storm_log)
        return env

    env["RMW_FASTRTPS_USE_QOS_FROM_XML"] = "1"
    env["FASTRTPS_DEFAULT_PROFILES_FILE"] = str(profile)
    env.pop("FASTDDS_DEFAULT_PROFILES_FILE", None)
    env.pop("FASTDDS_BUILTIN_TRANSPORTS", None)

    for name in tuple(env):
        if name.startswith("FASTDDS_CALM_"):
            env.pop(name, None)
    env["FASTDDS_CALM_ENABLED"] = "1" if calm_enabled else "0"
    env["FASTDDS_CALM_OBSERVER_ENABLED"] = "0"
    if calm_enabled:
        env["FASTDDS_CALM_LOG_DIR"] = str(run_dir)
        env["FASTDDS_CALM_HOLD_MODE"] = "1"
        env["FASTDDS_CALM_CONTROLLER"] = args.calm_controller
        env["FASTDDS_CALM_PACING_MS"] = str(args.calm_pacing_ms)
        env["FASTDDS_CALM41_PACING_MODE"] = args.calm41_pacing_mode
        env["FASTDDS_CALM41_PACING_ETA"] = str(args.calm41_pacing_eta)
        env["FASTDDS_CALM4_K_DEC"] = str(args.calm4_k_dec)
        env["FASTDDS_CALM4_K_INC"] = str(args.calm4_k_inc)
        env["FASTDDS_CALM4_ENTRY_F_THRESHOLD"] = str(
            args.calm_entry_f_threshold)
        env["FASTDDS_CALM4_ENTRY_F_MODE"] = args.calm_entry_f_mode
        env["FASTDDS_CALM41_TIMEOUT_RTT_MULTIPLIER"] = str(
            args.calm41_timeout_rtt_multiplier)
        env["FASTDDS_CALM_MAX_BUDGET_BYTES"] = str(args.calm_max_budget_bytes)
        env["FASTDDS_CALM_BUDGET_HORIZON_MS"] = str(args.calm_budget_horizon_ms)
        env["FASTDDS_CALM_MAX_SCHEDULED_BYTES"] = str(args.calm_max_scheduled_bytes)
        env["FASTDDS_CALM_STORM_INITIAL_BUDGET_BYTES"] = str(
            args.calm_initial_budget_bytes)
        env["FASTDDS_CALM_STORM_MIN_BUDGET_BYTES"] = str(
            args.calm_min_budget_bytes)
        env["FASTDDS_CALM_STORM_AI_BYTES"] = str(args.calm_ai_bytes)
        env["FASTDDS_CALM_MIN_BUDGET_SAMPLE_MULTIPLIER"] = str(
            args.calm_min_budget_sample_multiplier)
        env["FASTDDS_CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER"] = str(
            args.calm_initial_budget_sample_multiplier)
        env["FASTDDS_CALM_MAX_BUDGET_SAMPLE_MULTIPLIER"] = str(
            args.calm_max_budget_sample_multiplier)
        env["FASTDDS_CALM_AI_B_SAMPLE_MULTIPLIER"] = str(
            args.calm_ai_b_sample_multiplier)
        env["FASTDDS_CALM_BUDGET_SCALING_MODE"] = args.calm_budget_scaling_mode
        env["FASTDDS_CALM_STORM_GAMMA"] = str(args.calm_gamma)
        env["FASTDDS_CALM_STORM_RATE_GAMMA"] = str(args.calm_rate_gamma)
        env["FASTDDS_CALM_FIRST_FAILURE_GAMMA"] = str(
            args.calm_first_failure_gamma)
        env["FASTDDS_CALM_FIRST_FAILURE_RATE_GAMMA"] = str(
            args.calm_first_failure_rate_gamma)
        env["FASTDDS_CALM_STORM_RATE_AI_MBPS"] = str(args.calm_rate_ai_mbps)
        env["FASTDDS_CALM_ACK_INCREASE_MODE"] = args.calm_ack_increase_mode
        env["FASTDDS_CALM_RELEASE_RATE_MODE"] = args.calm_release_rate_mode
        env["FASTDDS_CALM_ACK_GOODPUT_HEADROOM"] = str(
            args.calm_ack_goodput_headroom)
        env["FASTDDS_CALM_MIN_REPAIR_RATE_MBPS"] = str(
            args.calm_min_release_rate_mbps)
        env["FASTDDS_CALM_INITIAL_REPAIR_RATE_MBPS"] = str(
            args.calm_initial_release_rate_mbps)
        env["FASTDDS_CALM_MAX_REPAIR_RATE_MBPS"] = str(
            args.calm_max_release_rate_mbps)
        env["FASTDDS_CALM_MIN_RATE_OFFERED_MULTIPLIER"] = str(
            args.calm_min_rate_offered_multiplier)
        env["FASTDDS_CALM_INITIAL_RATE_OFFERED_MULTIPLIER"] = str(
            args.calm_initial_rate_offered_multiplier)
        env["FASTDDS_CALM_MAX_RATE_OFFERED_MULTIPLIER"] = str(
            args.calm_max_rate_offered_multiplier)
        env["FASTDDS_CALM_AI_V_OFFERED_MULTIPLIER"] = str(
            args.calm_ai_v_offered_multiplier)
        env["FASTDDS_CALM_OFFERED_RATE_EWMA_ALPHA"] = str(
            args.calm_offered_rate_ewma_alpha)
        env["FASTDDS_CALM_SERVICE_RATE_MIN_MULTIPLIER"] = str(
            args.calm_service_rate_min_multiplier)
        env["FASTDDS_CALM_SERVICE_RATE_INITIAL_MULTIPLIER"] = str(
            args.calm_service_rate_initial_multiplier)
        env["FASTDDS_CALM_SERVICE_RATE_MAX_MULTIPLIER"] = str(
            args.calm_service_rate_max_multiplier)
        env["FASTDDS_CALM_SERVICE_RATE_AI_MULTIPLIER"] = str(
            args.calm_service_rate_ai_multiplier)
        env["FASTDDS_CALM_SERVICE_RATE_ALPHA_UP"] = str(
            args.calm_service_rate_alpha_up)
        env["FASTDDS_CALM_SERVICE_RATE_ALPHA_DOWN"] = str(
            args.calm_service_rate_alpha_down)
        env["FASTDDS_CALM_SERVICE_RATE_FAILURE_GAMMA"] = str(
            args.calm_service_rate_failure_gamma)
        env["FASTDDS_CALM_SERVICE_RATE_ACK_CAP_MULTIPLIER"] = str(
            args.calm_service_rate_ack_cap_multiplier)
        env["FASTDDS_CALM_PACING_DRAIN_GUARD"] = str(args.calm_pacing_drain_guard)
        env["FASTDDS_CALM_STORM_FEEDBACK_MIN_AGE_MS"] = str(
            args.calm_feedback_min_age_ms)
        env["FASTDDS_CALM_FEEDBACK_BYTE_PROGRESS"] = (
            "1" if args.fastdds_calm_feedback_byte_progress else "0")
        env["FASTDDS_CALM_FEEDBACK_GUARD_MODE"] = args.calm_feedback_guard_mode
        env["FASTDDS_CALM_FEEDBACK_GUARD_RTT_MULTIPLIER"] = str(
            args.calm_feedback_guard_rtt_multiplier)
        env["FASTDDS_CALM_FEEDBACK_GUARD_MIN_MS"] = str(
            args.calm_feedback_guard_min_ms)
        env["FASTDDS_CALM_FEEDBACK_GUARD_MAX_MS"] = str(
            args.calm_feedback_guard_max_ms)
        env["FASTDDS_CALM_FEEDBACK_RTT_ALPHA"] = str(args.calm_feedback_rtt_alpha)
        env["FASTDDS_CALM_RETRY_COOLDOWN_MS"] = str(
            args.calm_retry_cooldown_ms)
        env["FASTDDS_CALM_POST_REPAIR_GUARD_MS"] = str(
            args.calm_post_repair_guard_ms)
        env["FASTDDS_CALM_FAILED_ROUNDS_BEFORE_DECREASE"] = str(
            args.calm_failed_rounds_before_decrease)
        env["FASTDDS_CALM_MAX_ADMISSION_WINDOW_BYTES"] = str(
            args.calm_max_admission_window_bytes)
        env["FASTDDS_CALM_MIN_ADMISSION_WINDOW_BYTES"] = str(
            args.calm_min_admission_window_bytes)
        env["FASTDDS_CALM_DETECTOR_RHO_BYTES"] = str(args.calm_rho_bytes)
        env["FASTDDS_CALM_DETECTOR_RHO_SAMPLE_MULTIPLIER"] = str(
            args.calm_rho_sample_multiplier)
        env["FASTDDS_CALM_DETECTOR_RETRY_COUNT"] = str(args.calm_retry_count)
        env["FASTDDS_CALM_DETECTOR_RETRY_SIGNAL"] = args.calm_retry_signal
        env["FASTDDS_CALM_DETECTOR_FAILED_REPAIR_COUNT"] = str(
            args.calm_failure_count)
        env["FASTDDS_CALM_DETECTOR_GROWTH_ROUNDS"] = str(
            args.calm_growth_rounds)
        env["FASTDDS_CALM_DETECTOR_ACK_STALL_MS"] = str(
            args.calm_ack_stall_ms)
        env["FASTDDS_CALM_ACK_STALL_MODE"] = args.calm_ack_stall_mode
        env["FASTDDS_CALM_ACK_STALL_RTT_MULTIPLIER"] = str(
            args.calm_ack_stall_rtt_multiplier)
        env["FASTDDS_CALM_ACK_STALL_HB_MULTIPLIER"] = str(
            args.calm_ack_stall_hb_multiplier)
        env["FASTDDS_CALM_ACK_STALL_MIN_MS"] = str(args.calm_ack_stall_min_ms)
        env["FASTDDS_CALM_ACK_STALL_MAX_MS"] = str(args.calm_ack_stall_max_ms)
    if storm_log is None:
        env["FASTDDS_STORM_OBSERVER_ENABLED"] = "0"
        env.pop("FASTDDS_STORM_LOG_FILE", None)
    else:
        env["FASTDDS_STORM_OBSERVER_ENABLED"] = "1"
        env["FASTDDS_STORM_LOG_FILE"] = str(storm_log)
    return env


def generate_mode_profiles(
        *,
        mode: str,
        calm_enabled: bool,
        run_dir: Path,
        payload_bytes: int,
        hz: int,
        dds: str) -> tuple[Path, Path, dict[str, object]]:
    profile_dir = run_dir / "profiles"
    profile_dir.mkdir(parents=True, exist_ok=True)
    pub_profile = profile_dir / f"{mode}_pub.xml"
    sub_profile = profile_dir / f"{mode}_sub.xml"

    if dds == "cyclonedds":
        if mode != "opt12":
            raise ValueError("Cyclone DDS validation currently requires mode=opt12")
        parameters = compute_parameters(
            hz, payload_bytes, None, None, 1472,
            history_cache_enabled=False,
        )
        generate_cyclonedds_profiles(
            parameters,
            "127.0.0.1",
            "127.0.0.1",
            str(pub_profile),
            str(sub_profile),
        )
        manifest = asdict(parameters)
        manifest.update({
            "mode": mode,
            "dds": dds,
            "calm_enabled": calm_enabled,
            "heartbeat_optimization_enabled": True,
            "history_cache_optimization_enabled": False,
        })
    elif mode == "opt1":
        generate_opt1_profiles(
            "127.0.0.1",
            "127.0.0.1",
            str(pub_profile),
            str(sub_profile),
            1472,
        )
        manifest: dict[str, object] = {
            "mode": mode,
            "calm_enabled": calm_enabled,
            "max_message_size": 1472,
            "heartbeat_optimization_enabled": False,
            "heartbeat_period_ns": None,
            "history_cache_optimization_enabled": False,
        }
    else:
        parameters = compute_parameters(
            hz,
            payload_bytes,
            None,
            None,
            1472,
            history_cache_enabled=False,
        )
        generate_profiles(
            parameters,
            "127.0.0.1",
            "127.0.0.1",
            str(pub_profile),
            str(sub_profile),
        )
        manifest = asdict(parameters)
        manifest.update({
            "mode": mode,
            "calm_enabled": calm_enabled,
            "heartbeat_optimization_enabled": True,
            "history_cache_optimization_enabled": False,
        })

    with (profile_dir / f"{mode}_manifest.json").open(
            "w", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return pub_profile, sub_profile, manifest


def start_background(
        *,
        pub_env: dict[str, str],
        sub_env: dict[str, str],
        run_dir: Path,
        idx: int,
        payload_bytes: int,
        hz: int,
        max_samples: int,
        flows: int) -> list[ManagedProcess]:
    processes: list[ManagedProcess] = []
    bg_pub_env = pub_env.copy()
    bg_sub_env = sub_env.copy()
    for env in (bg_pub_env, bg_sub_env):
        env["FASTDDS_STORM_OBSERVER_ENABLED"] = "0"
        env.pop("FASTDDS_STORM_LOG_FILE", None)
        env["CYCLONEDDS_STORM_OBSERVER_ENABLED"] = "0"
        env["CYCLONEDDS_CALM_ENABLED"] = "0"
        env.pop("CYCLONEDDS_STORM_LOG_FILE", None)

    for flow in range(1, flows + 1):
        topic = f"bg{flow}"
        bg_idx = idx * 1000 + flow
        processes.append(ManagedProcess(
            ros_command(
                "calm_pretest_yw",
                "dds_sub",
                sub_args(
                    idx=bg_idx,
                    payload_bytes=payload_bytes,
                    count=100000,
                    max_samples=max_samples,
                    hz=hz,
                    topic=topic,
                    suffix="loopback_bg",
                    qos="best_effort",
                ),
            ),
            bg_sub_env,
            run_dir / f"{topic}_sub.log",
        ))
    time.sleep(1.0)
    for flow in range(1, flows + 1):
        topic = f"bg{flow}"
        bg_idx = idx * 1000 + flow
        processes.append(ManagedProcess(
            ros_command(
                "calm_pretest_yw",
                "dds_pub",
                main_pub_args(
                    idx=bg_idx,
                    payload_bytes=payload_bytes,
                    count=100000,
                    max_samples=max_samples,
                    hz=hz,
                    topic=topic,
                    trigger_after=100001,
                    qos="best_effort",
                ),
            ),
            bg_pub_env,
            run_dir / f"{topic}_pub.log",
        ))
    return processes


def float_values(rows: list[dict[str, str]], column: str) -> list[float]:
    values: list[float] = []
    for row in rows:
        try:
            values.append(float(row[column]))
        except (KeyError, TypeError, ValueError):
            pass
    return values


def read_process_stats(path: Path, prefix: str) -> dict[str, float]:
    stats: dict[str, float] = {}
    if not path.exists():
        return stats
    pattern = re.compile(rf"^\[{re.escape(prefix)}_STATS\]\s+([^:]+):\s+([-+0-9.eE]+)")
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = pattern.match(line.strip())
            if match is not None:
                try:
                    stats[match.group(1).strip()] = float(match.group(2))
                except ValueError:
                    pass
    return stats


def stream_calm_metrics(run_dir: Path) -> dict[str, float | int]:
    metrics: dict[str, float | int] = {
        "rho_max": 0,
        "rho_final": 0,
        "active_rows": 0,
        "budget_min": 0,
        "budget_max": 0,
        "rate_min": 0.0,
        "rate_max": 0.0,
        "service_rate_min": 0.0,
        "service_rate_max": 0.0,
        "service_rate_final": 0.0,
        "delivery_rate_max": 0.0,
        "transport_rate_max": 0.0,
        "dynamic_rate_min_final": 0.0,
        "dynamic_rate_initial_final": 0.0,
        "dynamic_rate_max_final": 0.0,
        "dynamic_rate_ai_final": 0.0,
        "service_success_rounds": 0,
        "service_failure_rounds": 0,
        "service_window_ms_final": 0.0,
        "service_window_acked_bytes_final": 0,
        "service_window_recovered_bytes_final": 0,
    }
    backlog_path = run_dir / "calm_backlog.csv"
    if backlog_path.exists():
        with backlog_path.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                try:
                    rho = int(float(row.get("rho_bytes", "0")))
                except ValueError:
                    rho = 0
                metrics["rho_max"] = max(int(metrics["rho_max"]), rho)
                metrics["rho_final"] = rho
                if row.get("control_state") == "CALM_ACTIVE":
                    metrics["active_rows"] = int(metrics["active_rows"]) + 1

    budget_path = run_dir / "calm_budget.csv"
    budget_values: list[int] = []
    rate_values: list[float] = []
    if budget_path.exists():
        with budget_path.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                if row.get("control_state") != "CALM_ACTIVE":
                    continue
                try:
                    budget_values.append(int(float(row.get("budget_bytes", "0"))))
                    rate_values.append(float(row.get("repair_rate_mbps", "0")))
                    service_rate = float(row.get("service_rate_mbps", "0"))
                    delivery_rate = float(row.get("delivery_rate_sample_mbps", "0"))
                    transport_rate = float(row.get("transport_rate_mbps", "0"))
                    metrics["service_rate_min"] = (
                        service_rate if service_rate > 0.0 and
                        float(metrics["service_rate_min"]) == 0.0 else
                        min(float(metrics["service_rate_min"]), service_rate)
                        if service_rate > 0.0 else metrics["service_rate_min"])
                    metrics["service_rate_max"] = max(
                        float(metrics["service_rate_max"]), service_rate)
                    metrics["service_rate_final"] = service_rate
                    metrics["delivery_rate_max"] = max(
                        float(metrics["delivery_rate_max"]), delivery_rate)
                    metrics["transport_rate_max"] = max(
                        float(metrics["transport_rate_max"]), transport_rate)
                    for source, target in (
                            ("dynamic_rate_min_mbps", "dynamic_rate_min_final"),
                            ("dynamic_rate_initial_mbps", "dynamic_rate_initial_final"),
                            ("dynamic_rate_max_mbps", "dynamic_rate_max_final"),
                            ("dynamic_rate_ai_mbps", "dynamic_rate_ai_final")):
                        metrics[target] = float(row.get(source, "0") or 0.0)
                    metrics["service_success_rounds"] = int(float(
                        row.get("service_success_rounds", "0") or 0.0))
                    metrics["service_failure_rounds"] = int(float(
                        row.get("service_failure_rounds", "0") or 0.0))
                    metrics["service_window_ms_final"] = float(
                        row.get("service_window_ms", "0") or 0.0)
                    metrics["service_window_acked_bytes_final"] = int(float(
                        row.get("service_window_acked_bytes", "0") or 0.0))
                    metrics["service_window_recovered_bytes_final"] = int(float(
                        row.get("service_window_recovered_bytes", "0") or 0.0))
                except ValueError:
                    pass
    positive_budgets = [value for value in budget_values if value > 0]
    positive_rates = [value for value in rate_values if value > 0.0]
    if positive_budgets:
        metrics["budget_min"] = min(positive_budgets)
        metrics["budget_max"] = max(positive_budgets)
    if positive_rates:
        metrics["rate_min"] = min(positive_rates)
        metrics["rate_max"] = max(positive_rates)
    return metrics


def stream_storm_metrics(path: Path | None) -> dict[str, object]:
    """Aggregate a potentially multi-gigabyte storm CSV in constant memory."""
    metrics: dict[str, object] = {
        "rows": 0,
        "rho_max": 0.0,
        "rho_final": 0.0,
        "oldest_repair_age_max": 0.0,
        "oldest_retransmit_count_max": 0.0,
        "oldest_failed_repair_count_max": 0.0,
        "max_failed_repair_count": 0.0,
        "oldest_no_progress_rounds_max": 0.0,
        "reader_acked_high_seq_final": 0.0,
        "ack_progress_age_max": 0.0,
        "admitted_bytes_max": 0.0,
        "admission_window_bytes_min": None,
        "calm_active_rows": 0,
        "calm_budget_min": None,
        "calm_budget_max": 0.0,
        "calm_release_rate_min": None,
        "calm_release_rate_max": 0.0,
        "calm_release_rate_final": 0.0,
        "calm_service_rate_min": None,
        "calm_service_rate_max": 0.0,
        "calm_service_rate_final": 0.0,
        "per_sample_retry": {},
        "per_sample_failure": {},
    }
    if path is None:
        return metrics

    def value(row: dict[str, str], column: str) -> float | None:
        try:
            return float(row[column])
        except (KeyError, TypeError, ValueError):
            return None

    per_sample_retry: dict[int, int] = metrics["per_sample_retry"]  # type: ignore[assignment]
    per_sample_failure: dict[int, int] = metrics["per_sample_failure"]  # type: ignore[assignment]
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            metrics["rows"] = int(metrics["rows"]) + 1
            for column, target in (
                    ("rho_bytes", "rho_max"),
                    ("oldest_repair_age_ms", "oldest_repair_age_max"),
                    ("oldest_repair_retransmit_count", "oldest_retransmit_count_max"),
                    ("oldest_repair_failed_repair_count", "oldest_failed_repair_count_max"),
                    ("max_failed_repair_count", "max_failed_repair_count"),
                    ("oldest_no_progress_rounds", "oldest_no_progress_rounds_max"),
                    ("ack_progress_age_ms", "ack_progress_age_max"),
                    ("admitted_bytes", "admitted_bytes_max")):
                current = value(row, column)
                if current is not None:
                    metrics[target] = max(float(metrics[target]), current)

            current_rho = value(row, "rho_bytes")
            if current_rho is not None:
                metrics["rho_final"] = current_rho
            acked_high = value(row, "reader_acked_high_seq")
            if acked_high is not None:
                metrics["reader_acked_high_seq_final"] = acked_high
            admission_window = value(row, "admission_window_bytes")
            if admission_window is not None:
                previous = metrics["admission_window_bytes_min"]
                metrics["admission_window_bytes_min"] = (
                    admission_window if previous is None else
                    min(float(previous), admission_window))
            if row.get("calm_state") == "CALM_ACTIVE":
                metrics["calm_active_rows"] = int(metrics["calm_active_rows"]) + 1

            for column, minimum_key, maximum_key in (
                    ("calm_window_bytes", "calm_budget_min", "calm_budget_max"),
                    ("release_rate_mbps", "calm_release_rate_min",
                     "calm_release_rate_max"),
                    ("service_rate_mbps", "calm_service_rate_min",
                     "calm_service_rate_max")):
                observed = value(row, column)
                if observed is None or observed <= 0:
                    continue
                previous_minimum = metrics[minimum_key]
                metrics[minimum_key] = (
                    observed if previous_minimum is None else
                    min(float(previous_minimum), observed))
                metrics[maximum_key] = max(float(metrics[maximum_key]), observed)
                if column == "release_rate_mbps":
                    metrics["calm_release_rate_final"] = observed
                elif column == "service_rate_mbps":
                    metrics["calm_service_rate_final"] = observed

            try:
                sequence = int(row["event_sample_seq"])
                retry_count = int(row["event_sample_retransmit_count"])
            except (KeyError, TypeError, ValueError):
                continue
            if sequence > 0:
                per_sample_retry[sequence] = max(
                    per_sample_retry.get(sequence, 0), retry_count)
                try:
                    failure_count = int(row["event_sample_failed_repair_count"])
                except (KeyError, TypeError, ValueError):
                    failure_count = 0
                per_sample_failure[sequence] = max(
                    per_sample_failure.get(sequence, 0), failure_count)
    return metrics


def summarize(
        run_dir: Path,
        *,
        mode: str,
        control: str,
        scenario: str,
        payload_kb: int,
        hz: int,
        requested_count: int,
        elapsed_s: float,
        timed_out: bool,
        baseline_loss_percent: float,
        case_a_loss_percent: float) -> dict[str, object]:
    pub_files = sorted(run_dir.glob("pub_*topicmain_*.csv"))
    sub_files = sorted(run_dir.glob("sub_*topicmain_*.csv"))
    pub_rows = read_csv(pub_files[-1]) if pub_files else []
    sub_rows = read_csv(sub_files[-1]) if sub_files else []
    storm_files = sorted(run_dir.glob("*dds_storm_*.csv"))
    storm_path = storm_files[-1] if storm_files else None
    storm = stream_storm_metrics(storm_path)
    calm = stream_calm_metrics(run_dir)
    sub_stats = read_process_stats(run_dir / "main_sub.log", "SUB")
    pub_stats = read_process_stats(run_dir / "main_pub.log", "PUB")

    pub_delays = float_values(pub_rows, "publish_delay_ms")
    sub_delays = float_values(sub_rows, "delay_ms")
    per_sample_retry: dict[int, int] = storm["per_sample_retry"]  # type: ignore[assignment]
    sample_retry_values = list(per_sample_retry.values())
    per_sample_failure: dict[int, int] = storm["per_sample_failure"]  # type: ignore[assignment]
    sample_failure_values = list(per_sample_failure.values())

    return {
        "dds": "cyclonedds" if storm_path is not None and
        storm_path.name.startswith("cyclonedds") else "fastdds",
        "mode": mode,
        "control": control,
        "scenario": scenario,
        "payload_kb": payload_kb,
        "hz": hz,
        "calm_enabled": int(control == "calm"),
        "max_message_size": 1472,
        "heartbeat_optimization": int(mode == "opt12"),
        "requested_count": requested_count,
        "published_count": len(pub_rows),
        "received_count": len(sub_rows),
        "receive_ratio": len(sub_rows) / requested_count if requested_count else 0.0,
        "elapsed_s": elapsed_s,
        "timed_out": int(timed_out),
        "baseline_loss_percent": baseline_loss_percent,
        "case_a_loss_percent": case_a_loss_percent if scenario == "case_a" else 0.0,
        "pub_delay_mean_ms": statistics.fmean(pub_delays) if pub_delays else 0.0,
        "pub_delay_p95_ms": percentile(pub_delays, 0.95),
        "pub_delay_max_ms": max(pub_delays, default=0.0),
        "sub_delay_mean_ms": statistics.fmean(sub_delays) if sub_delays else 0.0,
        "sub_delay_p95_ms": percentile(sub_delays, 0.95),
        "sub_delay_max_ms": max(sub_delays, default=0.0),
        "sub_delay_std_ms": sub_stats.get("std_delay", 0.0),
        "sub_actual_hz": sub_stats.get("hz_sub", 0.0),
        "pub_actual_hz": pub_stats.get("publish_rate", 0.0),
        "pub_interval_std_ms": pub_stats.get("std_interval", 0.0),
        "rho_max_bytes": max(int(storm["rho_max"]), int(calm["rho_max"])),
        "rho_final_bytes": int(calm["rho_final"]) if calm["rho_max"] else int(storm["rho_final"]),
        "oldest_repair_age_max_ms": float(storm["oldest_repair_age_max"]),
        "oldest_retransmit_count_max": int(storm["oldest_retransmit_count_max"]),
        "oldest_failed_repair_count_max": int(storm["oldest_failed_repair_count_max"]),
        "oldest_no_progress_rounds_max": int(storm["oldest_no_progress_rounds_max"]),
        "sample_retransmit_count_p50": percentile(sample_retry_values, 0.50),
        "sample_retransmit_count_p95": percentile(sample_retry_values, 0.95),
        "sample_retransmit_count_max": max(sample_retry_values, default=0),
        "sample_failed_repair_count_p50": percentile(sample_failure_values, 0.50),
        "sample_failed_repair_count_p95": percentile(sample_failure_values, 0.95),
        "sample_failed_repair_count_max": max(sample_failure_values, default=0),
        "max_failed_repair_count": int(storm["max_failed_repair_count"]),
        "samples_retransmitted": len(sample_retry_values),
        "ack_progress_age_max_ms": float(storm["ack_progress_age_max"]),
        "admitted_bytes_max": int(storm["admitted_bytes_max"]),
        "admission_window_bytes_min": int(storm["admission_window_bytes_min"] or 0),
        "calm_active_rows": max(int(storm["calm_active_rows"]), int(calm["active_rows"])),
        "calm_budget_min_bytes": int(calm["budget_min"] or
                                     storm["calm_budget_min"] or 0),
        "calm_budget_max_bytes": int(max(float(calm["budget_max"]),
                                         float(storm["calm_budget_max"]))),
        "calm_release_rate_min_mbps": float(calm["rate_min"] or
                                             storm["calm_release_rate_min"] or 0),
        "calm_release_rate_max_mbps": max(float(calm["rate_max"]),
                                           float(storm["calm_release_rate_max"])),
        "calm_release_rate_final_mbps": float(
            storm["calm_release_rate_final"] or calm["rate_max"] or 0),
        "calm_service_rate_min_mbps": float(calm["service_rate_min"] or
                                             storm["calm_service_rate_min"] or 0),
        "calm_service_rate_max_mbps": max(float(calm["service_rate_max"]),
                                           float(storm["calm_service_rate_max"])),
        "calm_service_rate_final_mbps": float(calm["service_rate_final"] or
                                               storm["calm_service_rate_final"]),
        "calm_delivery_rate_max_mbps": float(calm["delivery_rate_max"]),
        "calm_transport_rate_max_mbps": float(calm["transport_rate_max"]),
        "calm_dynamic_rate_min_final_mbps": float(calm["dynamic_rate_min_final"]),
        "calm_dynamic_rate_initial_final_mbps": float(
            calm["dynamic_rate_initial_final"]),
        "calm_dynamic_rate_max_final_mbps": float(calm["dynamic_rate_max_final"]),
        "calm_dynamic_rate_ai_final_mbps": float(calm["dynamic_rate_ai_final"]),
        "calm_service_success_rounds": int(calm["service_success_rounds"]),
        "calm_service_failure_rounds": int(calm["service_failure_rounds"]),
        "calm_service_window_ms_final": float(calm["service_window_ms_final"]),
        "calm_service_window_acked_bytes_final": int(
            calm["service_window_acked_bytes_final"]),
        "calm_service_window_recovered_bytes_final": int(
            calm["service_window_recovered_bytes_final"]),
        "reader_acked_high_seq_final": int(storm["reader_acked_high_seq_final"]),
        "storm_event_rows": int(storm["rows"]),
        "storm_csv": str(storm_path) if storm_path is not None else "",
        "run_dir": str(run_dir),
    }


def write_summary(
        summary_path: Path,
        rows: list[dict[str, object]]) -> None:
    """Persist every completed run so a reboot cannot erase batch progress."""
    if not rows:
        return

    temporary_path = summary_path.with_suffix(".csv.tmp")
    with temporary_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    temporary_path.replace(summary_path)


def run_one(
        *,
        batch_dir: Path,
        run_number: int,
        domain_id: int,
        mode: str,
        control: str,
        scenario: str,
        payload_kb: int,
        hz: int,
        case_a_loss_percent: float,
        args: argparse.Namespace) -> dict[str, object]:
    suffix = f"p{payload_kb}_h{hz}_{mode}_{control}_{scenario}"
    if scenario == "case_a":
        suffix += f"_loss{case_a_loss_percent:g}pct"
    run_dir = batch_dir / f"{run_number:03d}_{suffix}"
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "ros_log").mkdir(exist_ok=True)

    payload_bytes = payload_kb * 1024
    pub_profile, sub_profile, manifest = generate_mode_profiles(
        mode=mode,
        calm_enabled=control == "calm",
        run_dir=run_dir,
        payload_bytes=payload_bytes,
        hz=hz,
        dds=args.dds,
    )
    # Start every run from the same baseline. CASE A loss is injected only
    # after the main writer has published trigger_after samples, so discovery
    # loss cannot be mistaken for failed reliable recovery.
    effective_loss = args.baseline_loss_percent
    if not args.no_qdisc:
        configure_qdisc(
            rate_mbit=args.link_rate_mbit,
            delay_ms=args.delay_ms,
            jitter_ms=args.jitter_ms,
            loss_percent=effective_loss,
            limit=args.qdisc_limit,
        )

    storm_log = (
        None if args.no_storm_log
        else run_dir / (
            f"cyclonedds_storm_{suffix}.csv" if args.dds == "cyclonedds"
            else f"fastdds_storm_{suffix}.csv")
    )
    pub_env = process_env(
        run_dir=run_dir,
        domain_id=domain_id,
        profile=pub_profile,
        storm_log=storm_log,
        calm_enabled=control == "calm",
        args=args,
    )
    sub_env = process_env(
        run_dir=run_dir,
        domain_id=domain_id,
        profile=sub_profile,
        storm_log=None,
        calm_enabled=False,
        args=args,
    )
    # Reliable recovery must not be mistaken for completion merely because no
    # new sample arrived for the subscriber's default 30-second idle timeout.
    sub_env["CALM_SUB_INACTIVE_TIMEOUT_S"] = str(args.timeout_s + 60.0)

    topic = "main"
    sub = ManagedProcess(
        ros_command(
            "calm_pretest_yw",
            "dds_sub",
            sub_args(
                idx=run_number,
                payload_bytes=payload_bytes,
                count=args.count,
                max_samples=args.max_samples,
                hz=hz,
                topic=topic,
                suffix=suffix,
                qos_depth=args.qos_depth,
            ),
        ),
        sub_env,
        run_dir / "main_sub.log",
    )
    time.sleep(args.discovery_wait_s)
    pub = ManagedProcess(
        ros_command(
            "calm_pretest_yw",
            "dds_pub",
            main_pub_args(
                idx=run_number,
                payload_bytes=payload_bytes,
                count=args.count,
                max_samples=args.max_samples,
                hz=hz,
                topic=topic,
                trigger_after=args.trigger_after,
                qos_depth=args.qos_depth,
            ),
        ),
        pub_env,
        run_dir / "main_pub.log",
    )

    started = time.monotonic()
    disturbance_done = scenario == "normal"
    background: list[ManagedProcess] = []
    timed_out = False
    try:
        while True:
            elapsed = time.monotonic() - started
            if elapsed >= args.timeout_s:
                timed_out = True
                break
            if not disturbance_done and pub.trigger.is_set():
                if scenario == "case_d":
                    if not args.no_qdisc:
                        configure_qdisc(
                            rate_mbit=args.link_rate_mbit,
                            delay_ms=args.delay_ms,
                            jitter_ms=args.jitter_ms,
                            loss_percent=100.0,
                            limit=args.qdisc_limit,
                        )
                    else:
                        sub.signal_group(signal.SIGSTOP)
                    time.sleep(args.case_duration_s)
                    if not args.no_qdisc:
                        configure_qdisc(
                            rate_mbit=args.link_rate_mbit,
                            delay_ms=args.delay_ms,
                            jitter_ms=args.jitter_ms,
                            loss_percent=args.baseline_loss_percent,
                            limit=args.qdisc_limit,
                        )
                    else:
                        sub.signal_group(signal.SIGCONT)
                elif scenario == "case_l":
                    background = start_background(
                        pub_env=pub_env,
                        sub_env=sub_env,
                        run_dir=run_dir,
                        idx=run_number,
                        payload_bytes=args.bg_payload_kb * 1024,
                        hz=args.bg_hz,
                        max_samples=args.max_samples,
                        flows=args.bg_flows,
                    )
                    time.sleep(args.case_duration_s)
                    for process in background:
                        process.terminate()
                    background.clear()
                elif scenario == "case_a" and not args.no_qdisc:
                    configure_qdisc(
                        rate_mbit=args.link_rate_mbit,
                        delay_ms=args.delay_ms,
                        jitter_ms=args.jitter_ms,
                        loss_percent=case_a_loss_percent,
                        limit=args.qdisc_limit,
                    )
                    if args.case_a_duration_s > 0.0:
                        time.sleep(args.case_a_duration_s)
                        configure_qdisc(
                            rate_mbit=args.link_rate_mbit,
                            delay_ms=args.delay_ms,
                            jitter_ms=args.jitter_ms,
                            loss_percent=args.baseline_loss_percent,
                            limit=args.qdisc_limit,
                        )
                disturbance_done = True

            if sub.poll() is not None:
                break
            if pub.poll() is not None and elapsed > args.count / hz + 15.0:
                # The writer has left, so no additional repair can be emitted.
                break
            time.sleep(0.1)
    finally:
        for process in background:
            process.terminate()
        pub_exit = pub.terminate()
        sub_exit = sub.terminate()
        if not args.no_qdisc:
            configure_qdisc(
                rate_mbit=args.link_rate_mbit,
                delay_ms=args.delay_ms,
                jitter_ms=args.jitter_ms,
                loss_percent=args.baseline_loss_percent,
                limit=args.qdisc_limit,
            )

    elapsed_s = time.monotonic() - started
    row = summarize(
        run_dir,
        mode=mode,
        control=control,
        scenario=scenario,
        payload_kb=payload_kb,
        hz=hz,
        requested_count=args.count,
        elapsed_s=elapsed_s,
        timed_out=timed_out,
        baseline_loss_percent=args.baseline_loss_percent,
        case_a_loss_percent=case_a_loss_percent,
    )
    row.update({
        "dds": args.dds,
        "qos_depth": args.qos_depth,
        "run_number": run_number,
        "domain_id": domain_id,
        "pub_exit": pub_exit,
        "sub_exit": sub_exit,
        "disturbance_done": int(disturbance_done),
        "heartbeat_period_ns": manifest.get("heartbeat_period_ns"),
        "calm_controller": args.calm_controller if control == "calm" else "off",
        "calm_entry_f_threshold": (
            args.calm_entry_f_threshold if control == "calm" else 0),
        "calm_entry_f_mode": (
            args.calm_entry_f_mode if control == "calm" else "off"),
        "calm42_pacing_source": (
            "writer_heartbeat" if control == "calm" and
            args.calm_controller in ("calm42", "calm43") else "not_applicable"),
        "calm_feedback_byte_progress": int(
            args.calm_feedback_byte_progress if args.dds == "cyclonedds"
            else args.fastdds_calm_feedback_byte_progress),
    })
    return row


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run paired Default/CALM OPT1 and OPT1+2 loopback tests."
        )
        )
    parser.add_argument(
        "--dds", choices=("fastdds", "cyclonedds"), default="fastdds")
    parser.add_argument(
        "--result-root", default="/home/csi/ros2_ws/results/test_yw_2")
    parser.add_argument("--label", default="default_compare")
    parser.add_argument("--modes", default="opt1,opt12")
    parser.add_argument(
        "--controls",
        default="default",
        help="comma-separated default/calm variants")
    parser.add_argument("--scenarios", default="normal,case_l,case_d,case_a")
    parser.add_argument("--payload-kb", type=int, default=1024)
    parser.add_argument("--hz", type=int, default=10)
    parser.add_argument(
        "--payloads-kb",
        help="comma-separated payload sweep; overrides --payload-kb")
    parser.add_argument(
        "--hertz",
        help="comma-separated rate sweep; overrides --hz")
    parser.add_argument("--count", type=int, default=2000)
    parser.add_argument("--max-samples", type=int, default=400)
    parser.add_argument(
        "--qos-depth",
        type=int,
        default=1,
        help=("KEEP_LAST depth for best-effort runs; Reliable main traffic "
              "uses KEEP_ALL"))
    parser.add_argument("--trigger-after", type=int, default=200)
    parser.add_argument("--case-duration-s", type=float, default=20.0)
    parser.add_argument("--case-a-loss-percent", type=float, default=5.0)
    parser.add_argument(
        "--case-a-duration-s",
        type=float,
        default=0.0,
        help="Restore baseline loss after this many seconds; zero keeps loss until run end.")
    parser.add_argument(
        "--case-a-losses",
        help="comma-separated CASE A sweep; overrides --case-a-loss-percent")
    parser.add_argument("--bg-payload-kb", type=int, default=2048)
    parser.add_argument("--bg-hz", type=int, default=30)
    parser.add_argument("--bg-flows", type=int, default=5)
    parser.add_argument("--timeout-s", type=float, default=300.0)
    parser.add_argument("--discovery-wait-s", type=float, default=1.5)
    parser.add_argument("--domain-base", type=int, default=190)
    parser.add_argument("--link-rate-mbit", type=int, default=180)
    parser.add_argument("--delay-ms", type=float, default=3.0)
    parser.add_argument("--jitter-ms", type=float, default=1.0)
    parser.add_argument("--baseline-loss-percent", type=float, default=0.0)
    parser.add_argument(
        "--qdisc-limit",
        type=int,
        default=0,
        help="netem packet limit; 0 omits the option and uses netem default")
    parser.add_argument("--no-qdisc", action="store_true")
    parser.add_argument(
        "--no-storm-log",
        action="store_true",
        help=("Disable the high-volume per-event storm CSV. CALM controller "
              "state and delay/result CSVs remain enabled."))
    parser.add_argument("--calm-controller", default="calm4")
    parser.add_argument("--calm-pacing-ms", type=float, default=50.0)
    parser.add_argument(
        "--calm41-pacing-mode",
        choices=("fixed", "entry_ack"),
        default="fixed",
        help=("CALM 4.1 pacing: fixed fallback, or eta*B_initial/mu_entry "
              "using pre-entry cumulative-ACK goodput"))
    parser.add_argument("--calm41-pacing-eta", type=float, default=1.10)
    parser.add_argument("--calm4-k-dec", type=float, default=0.25)
    parser.add_argument("--calm4-k-inc", type=float, default=0.25)
    parser.add_argument(
        "--calm-entry-f-threshold",
        type=int,
        choices=(1, 2, 3),
        default=1,
        help=("Absolute F_old entry threshold used by legacy absolute_f mode; "
              "delta_f mode always uses delta F_old >= 1."))
    parser.add_argument(
        "--calm-entry-f-mode",
        choices=("delta_f", "absolute_f"),
        default="delta_f",
        help=("Use the current feedback-round delta F_old for CALM 4.2 entry "
              "and decrease. absolute_f exists only to reproduce the earlier "
              "F=1/2/3 threshold sweep."))
    parser.add_argument("--calm41-timeout-rtt-multiplier", type=float, default=4.0)
    parser.add_argument("--calm-max-budget-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--calm-initial-budget-bytes", type=int, default=512 * 1024)
    parser.add_argument("--calm-min-budget-bytes", type=int, default=128 * 1024)
    parser.add_argument("--calm-budget-horizon-ms", type=float, default=20.0)
    parser.add_argument("--calm-max-scheduled-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--calm-ai-bytes", type=int, default=256 * 1024)
    parser.add_argument("--calm-min-budget-sample-multiplier", type=float, default=0.25)
    parser.add_argument("--calm-initial-budget-sample-multiplier", type=float, default=1.0)
    parser.add_argument("--calm-max-budget-sample-multiplier", type=float, default=2.0)
    parser.add_argument("--calm-ai-b-sample-multiplier", type=float, default=0.25)
    parser.add_argument(
        "--calm-budget-scaling-mode",
        choices=("replace", "floor"),
        default="floor")
    parser.add_argument("--calm-gamma", type=float, default=0.75)
    parser.add_argument("--calm-rate-gamma", type=float, default=0.875)
    parser.add_argument("--calm-first-failure-gamma", type=float, default=0.80)
    parser.add_argument(
        "--calm-first-failure-rate-gamma", type=float, default=0.90)
    parser.add_argument("--calm-rate-ai-mbps", type=float, default=16.0)
    parser.add_argument(
        "--calm-ack-increase-mode",
        choices=("event", "byte_normalized"),
        default="event")
    parser.add_argument(
        "--calm-release-rate-mode",
        choices=("fixed", "ack_goodput", "dds_service"),
        default="fixed")
    parser.add_argument("--calm-ack-goodput-headroom", type=float, default=1.05)
    parser.add_argument("--calm-min-release-rate-mbps", type=float, default=64.0)
    parser.add_argument("--calm-initial-release-rate-mbps", type=float, default=192.0)
    parser.add_argument("--calm-max-release-rate-mbps", type=float, default=192.0)
    parser.add_argument("--calm-min-rate-offered-multiplier", type=float, default=0.0)
    parser.add_argument("--calm-initial-rate-offered-multiplier", type=float, default=0.0)
    parser.add_argument("--calm-max-rate-offered-multiplier", type=float, default=0.0)
    parser.add_argument("--calm-ai-v-offered-multiplier", type=float, default=0.0)
    parser.add_argument("--calm-offered-rate-ewma-alpha", type=float, default=0.2)
    parser.add_argument("--calm-service-rate-min-multiplier", type=float, default=0.25)
    parser.add_argument("--calm-service-rate-initial-multiplier", type=float, default=1.0)
    parser.add_argument("--calm-service-rate-max-multiplier", type=float, default=2.0)
    parser.add_argument("--calm-service-rate-ai-multiplier", type=float, default=0.25)
    parser.add_argument("--calm-service-rate-alpha-up", type=float, default=0.25)
    parser.add_argument("--calm-service-rate-alpha-down", type=float, default=0.10)
    parser.add_argument("--calm-service-rate-failure-gamma", type=float, default=0.98)
    parser.add_argument("--calm-service-rate-ack-cap-multiplier", type=float, default=1.05)
    parser.add_argument("--calm-pacing-drain-guard", type=float, default=1.10)
    parser.add_argument("--calm-feedback-min-age-ms", type=float, default=100.0)
    parser.add_argument(
        "--cyclonedds-calm-feedback-min-age-ms",
        type=float,
        default=10.0,
        help=(
            "CycloneDDS-specific feedback guard. OPT2 feedback is commonly "
            "about 20 ms apart, so this remains separate from Fast DDS's "
            "tested 100 ms guard."))
    parser.add_argument(
        "--calm-feedback-byte-progress",
        type=int,
        choices=(0, 1),
        default=1,
        help=("Scale failure attenuation by the remaining requested fragment "
              "bytes in comparable feedback; zero preserves absolute-F behavior."))
    parser.add_argument(
        "--fastdds-calm-feedback-byte-progress",
        type=int,
        choices=(0, 1),
        default=1,
        help=("Scale CALM 4 decrease by the fraction of transmitted repair bytes "
              "that valid feedback requests again."))
    parser.add_argument(
        "--calm-feedback-guard-mode",
        choices=("fixed", "dynamic"),
        default="fixed")
    parser.add_argument(
        "--calm-feedback-guard-rtt-multiplier", type=float, default=1.0)
    parser.add_argument("--calm-feedback-guard-min-ms", type=float, default=10.0)
    parser.add_argument("--calm-feedback-guard-max-ms", type=float, default=2000.0)
    parser.add_argument("--calm-feedback-rtt-alpha", type=float, default=0.2)
    parser.add_argument("--calm-retry-cooldown-ms", type=float, default=100.0)
    parser.add_argument("--calm-post-repair-guard-ms", type=float, default=0.0)
    parser.add_argument(
        "--calm-failed-rounds-before-decrease", type=int, default=1)
    parser.add_argument(
        "--calm-max-admission-window-bytes", type=int, default=8 * 1024 * 1024)
    parser.add_argument(
        "--calm-min-admission-window-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--calm-rho-bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument(
        "--calm-rho-sample-multiplier",
        type=float,
        default=2.0,
        help=("Use multiplier times the ReaderProxy-observed mean serialized "
              "sample size as the rho threshold; the tested default is 2.0, "
              "and zero keeps --calm-rho-bytes."))
    parser.add_argument("--calm-retry-count", type=int, default=2)
    parser.add_argument(
        "--calm-retry-signal", choices=("n", "f", "rho_progress"), default="rho_progress",
        help=("Select actual repair rounds N_i, confirmed failed feedback F_i, "
              "or activate from rho/progress and use F_i only for control gain."))
    parser.add_argument("--calm-failure-count", type=int, default=2)
    parser.add_argument("--calm-growth-rounds", type=int, default=2)
    parser.add_argument("--calm-ack-stall-ms", type=float, default=500.0)
    parser.add_argument(
        "--calm-ack-stall-mode",
        choices=("fixed", "dynamic"),
        default="fixed")
    parser.add_argument("--calm-ack-stall-rtt-multiplier", type=float, default=4.0)
    parser.add_argument("--calm-ack-stall-hb-multiplier", type=float, default=2.0)
    parser.add_argument("--calm-ack-stall-min-ms", type=float, default=50.0)
    parser.add_argument("--calm-ack-stall-max-ms", type=float, default=2000.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    modes = comma_list(args.modes)
    controls = comma_list(args.controls)
    scenarios = comma_list(args.scenarios)
    payloads_kb = (
        comma_ints(args.payloads_kb)
        if args.payloads_kb
        else [args.payload_kb]
    )
    hertz = comma_ints(args.hertz) if args.hertz else [args.hz]
    case_a_losses = (
        comma_floats(args.case_a_losses)
        if args.case_a_losses
        else [args.case_a_loss_percent]
    )
    invalid_modes = sorted(set(modes) - set(MODES))
    invalid_controls = sorted(set(controls) - set(CONTROLS))
    invalid_scenarios = sorted(set(scenarios) - set(SCENARIOS))
    if invalid_modes:
        raise SystemExit(f"unsupported modes: {', '.join(invalid_modes)}")
    if args.dds == "cyclonedds" and modes != ["opt12"]:
        raise SystemExit("Cyclone DDS runs require --modes opt12")
    if invalid_controls:
        raise SystemExit(f"unsupported controls: {', '.join(invalid_controls)}")
    if invalid_scenarios:
        raise SystemExit(f"unsupported scenarios: {', '.join(invalid_scenarios)}")
    scenario_variants = sum(
        len(case_a_losses) if scenario == "case_a" else 1
        for scenario in scenarios
    )
    total_runs = (
        len(payloads_kb) * len(hertz) * len(modes) * len(controls)
        * scenario_variants
    )
    if not 0 <= args.domain_base <= 232:
        raise SystemExit("domain-base must be within 0..232")
    if not 0 <= args.baseline_loss_percent <= 100:
        raise SystemExit("baseline loss must be within 0..100")
    if not case_a_losses or any(
            loss < 0 or loss > 100 for loss in case_a_losses):
        raise SystemExit("every CASE A loss must be within 0..100")
    if args.case_a_duration_s < 0:
        raise SystemExit("CASE A duration must be zero or positive")
    if args.qdisc_limit < 0:
        raise SystemExit("qdisc-limit must be zero or positive")
    if args.qos_depth <= 0:
        raise SystemExit("qos-depth must be positive")

    batch_dir = (
        Path(args.result_root).resolve()
        / f"DDSOPT_loopback_{args.dds}_{args.label}_{timestamp()}"
    )
    batch_dir.mkdir(parents=True, exist_ok=True)
    with (batch_dir / "parameters.json").open("w", encoding="utf-8") as stream:
        json.dump(vars(args), stream, indent=2, sort_keys=True)
        stream.write("\n")

    sudo_stop: threading.Event | None = None
    sudo_thread: threading.Thread | None = None
    if not args.no_qdisc and os.geteuid() != 0:
        permission_probe = subprocess.run(
            ["sudo", "-n", "tc", "qdisc", "show", "dev", "lo"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if permission_probe.returncode != 0:
            subprocess.run(["sudo", "-v"], check=True)
        sudo_stop = threading.Event()
        sudo_thread = threading.Thread(
            target=keep_sudo_ticket, args=(sudo_stop,), daemon=True)
        sudo_thread.start()

    rows: list[dict[str, object]] = []
    summary_path = batch_dir / "summary.csv"
    run_number = 1
    try:
        # Pair the two modes inside each scenario so their channel conditions
        # are as close in time as possible.
        for payload_kb in payloads_kb:
            for hz in hertz:
                for scenario in scenarios:
                    losses = case_a_losses if scenario == "case_a" else [0.0]
                    for case_a_loss_percent in losses:
                        for mode in modes:
                            for control in controls:
                                print(
                                    f"[DDSOPT-LO] run={run_number}/{total_runs} "
                                    f"payload={payload_kb}KiB hz={hz} mode={mode} "
                                    f"scenario={scenario} "
                                    f"loss={case_a_loss_percent:g}% control={control}",
                                    flush=True,
                                )
                                domain_span = 233 - args.domain_base
                                domain_id = args.domain_base + (
                                    (run_number - 1) % domain_span)
                                row = run_one(
                                    batch_dir=batch_dir,
                                    run_number=run_number,
                                    domain_id=domain_id,
                                    mode=mode,
                                    control=control,
                                    scenario=scenario,
                                    payload_kb=payload_kb,
                                    hz=hz,
                                    case_a_loss_percent=case_a_loss_percent,
                                    args=args,
                                )
                                rows.append(row)
                                write_summary(summary_path, rows)
                                print(
                                    f"[DDSOPT-LO] recv={row['received_count']}/"
                                    f"{row['requested_count']} "
                                    f"p95={row['sub_delay_p95_ms']:.1f}ms "
                                    f"rho_max={row['rho_max_bytes']} "
                                    f"timeout={row['timed_out']}",
                                    flush=True,
                                )
                                run_number += 1
                                time.sleep(1.0)
    finally:
        if not args.no_qdisc:
            remove_qdisc()
        if sudo_stop is not None:
            sudo_stop.set()
        if sudo_thread is not None:
            sudo_thread.join(timeout=1.0)

    write_summary(summary_path, rows)
    print(f"[DDSOPT-LO] results: {batch_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
