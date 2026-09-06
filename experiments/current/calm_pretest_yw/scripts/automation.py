#!/usr/bin/env python3
"""
Auto-experiment runner for ROS 2 DDS throughput tests
"""

import os
import csv
import re
import shlex
import subprocess
import sys
import time
from itertools import product
from typing import List, Tuple
import threading

from dds_wireless_optimizer import (
    compute_parameters,
    generate_cyclonedds_profiles,
    generate_profiles,
)

DEBUG_VERBOSE = True
RESULT_DIR = os.getenv(
    "CALM_EXPERIMENT_RESULT_DIR", "/home/csi/ros2_ws/results/test_yw_2"
)
REMOTE_RESULT_DIR = os.getenv(
    "CALM_REMOTE_RESULT_DIR", "/home/csilab/ros2_ws/results/test_yw_2"
)


def debug_log(message: str) -> None:
    if DEBUG_VERBOSE:
        try:
            print(f"[DEBUG] {message}")
        except Exception:
            pass


def stream_output(prefix, pipe, output_list):

    try:
        for line in iter(pipe.readline, ''):
            clean = line.rstrip('\r\n')
            if "Connection to" in clean and "closed" in clean:
                continue
            if clean.strip():
                # PUB/SUB: 시작 소켓 목록 + 완료 후 소켓 목록까지 포함 (최대 35줄)
                output_list.append(clean)
                if len(output_list) <= 35:
                    debug_log(f"{prefix} first lines[{len(output_list)}]: {clean[:200]}")
    except Exception as e:
        pass


# PAYLOAD_SIZES_KB = [8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384]
PAYLOAD_SIZES_KB = [4096, 2048, 1024, 512, 256, 128]
# PAYLOAD_SIZES_KB = [8192, 4096, 2048, 1024, 512, 256, 128, 64, 32, 16, 8]
PAYLOAD_SIZES_BYTES = [size * 1024 for size in PAYLOAD_SIZES_KB]

def env_float(name: str, default: float) -> float:
    try:
        return float(os.getenv(name, str(default)))
    except ValueError:
        return default


def env_int(name: str, default: int) -> int:
    try:
        return int(os.getenv(name, str(default)))
    except ValueError:
        return default


def env_int_list(name: str, default: List[int]) -> List[int]:
    raw_value = os.getenv(name)
    if raw_value is None:
        return list(default)
    try:
        values = [int(value.strip()) for value in raw_value.split(",") if value.strip()]
    except ValueError as error:
        raise RuntimeError(f"Invalid comma-separated integer list in {name}: {raw_value}") from error
    if not values:
        raise RuntimeError(f"{name} must contain at least one integer")
    return values


# CALM budget-control parameters. The Wi-Fi autotuner overrides these through
# the same FASTDDS_CALM_* variables that are passed to the writer process.
# CALM_BETA_LIST: List[float] = [0, 0.2, 0.4, 0.6, 0.8, 1.0]
# CALM_GAMMA_LIST: List[float] = [0, 0.25, 0.5, 0.75, 1.0]
# CALM_ALPHA_LIST: List[float] = [1, 2, 3, 4, 5]
# CALM_Q_MIN_LIST: List[float] = [0.01, 0.02, 0.03, 0.04, 0.05]
# CALM 3.0 defaults selected by the Ethernet wireless-emulation comparison.
# BETA is retained only for existing result-name compatibility.
CALM_BETA_LIST: List[float] = [0.8]
CALM_GAMMA_LIST: List[float] = [env_float("FASTDDS_CALM_GAMMA", 0.5)]
CALM_ALPHA_LIST: List[float] = [env_float("FASTDDS_CALM_ALPHA", 64.0)]
CALM_Q_MIN_LIST: List[float] = [env_float("FASTDDS_CALM_Q_MIN", 0.125)]
CALM_CONTROLLER: str = os.getenv("FASTDDS_CALM_CONTROLLER", "calm4")
CALM_PACING_MS: float = env_float("FASTDDS_CALM_PACING_MS", 50.0)
CALM4_K_DEC: float = env_float("FASTDDS_CALM4_K_DEC", 0.25)
CALM4_K_INC: float = env_float("FASTDDS_CALM4_K_INC", 0.25)
CALM_FRAGMENT_BYTES_PER_PERIOD: int = env_int("FASTDDS_CALM_FRAGMENT_BYTES_PER_PERIOD", 0)
CALM_FRAGMENT_PERIOD_MS: int = env_int("FASTDDS_CALM_FRAGMENT_PERIOD_MS", 1)
CALM_MAX_RTPS_MESSAGE_SIZE: int = env_int("FASTDDS_CALM_MAX_RTPS_MESSAGE_SIZE", 0)
CALM_MAX_BATCH_BYTES: int = env_int("FASTDDS_CALM_MAX_BUDGET_BYTES", 2 * 1024 * 1024)
CALM_MAX_SCHEDULED_BYTES: int = env_int(
    "FASTDDS_CALM_MAX_SCHEDULED_BYTES", 2 * 1024 * 1024
)
CALM_BUDGET_HORIZON_MS: float = env_float("FASTDDS_CALM_BUDGET_HORIZON_MS", 20.0)
CALM_DELTA_THRESHOLD_BYTES: int = env_int(
    "FASTDDS_CALM_DELTA_THRESHOLD_BYTES", 16 * 1024
)
CALM_ONSET_THRESHOLD_BYTES: int = env_int(
    "FASTDDS_CALM_ONSET_THRESHOLD_BYTES", 2 * 1024 * 1024
)
CALM_MIN_REPAIR_RATE_MBPS: float = env_float("FASTDDS_CALM_MIN_REPAIR_RATE_MBPS", 64.0)
CALM_INITIAL_REPAIR_RATE_MBPS: float = env_float(
    "FASTDDS_CALM_INITIAL_REPAIR_RATE_MBPS", 192.0
)
CALM_MAX_REPAIR_RATE_MBPS: float = env_float("FASTDDS_CALM_MAX_REPAIR_RATE_MBPS", 192.0)
CALM_HELD_NEW_RATE_MBPS: float = env_float("FASTDDS_CALM_HELD_NEW_RATE_MBPS", 160.0)
CALM_RETRY_COOLDOWN_MS: float = env_float("FASTDDS_CALM_RETRY_COOLDOWN_MS", 100.0)
CALM_FIRST_REPAIR_DELAY_MS: float = env_float("FASTDDS_CALM_FIRST_REPAIR_DELAY_MS", 25.0)
CALM_POST_REPAIR_GUARD_MS: float = env_float("FASTDDS_CALM_POST_REPAIR_GUARD_MS", 0.0)
CALM_RECOVERY_PROBE_DELAY_MS: float = env_float(
    "FASTDDS_CALM_RECOVERY_PROBE_DELAY_MS", 150.0
)
CALM_MIN_PATH_RATE_MBPS: float = env_float("FASTDDS_CALM_MIN_PATH_RATE_MBPS", 16.0)
CALM_PATH_GAMMA: float = env_float("FASTDDS_CALM_PATH_GAMMA", 0.75)
CALM_PATH_ALPHA: float = env_float("FASTDDS_CALM_PATH_ALPHA", 16.0)
# CALM experiments always use the complete shared-budget policy.
CALM_HOLD_MODE: int = 1
CALM_KP: float = env_float("FASTDDS_CALM_KP", 0.30)
CALM_KI: float = env_float("FASTDDS_CALM_KI", 0.08)
CALM_KD: float = env_float("FASTDDS_CALM_KD", 0.75)
CALM_STORM_INITIAL_BUDGET_BYTES: int = env_int(
    "FASTDDS_CALM_STORM_INITIAL_BUDGET_BYTES", 512 * 1024
)
CALM_STORM_MIN_BUDGET_BYTES: int = env_int(
    "FASTDDS_CALM_STORM_MIN_BUDGET_BYTES", 128 * 1024
)
CALM_STORM_AI_BYTES: int = env_int("FASTDDS_CALM_STORM_AI_BYTES", 256 * 1024)
CALM_MIN_BUDGET_SAMPLE_MULTIPLIER: float = env_float(
    "FASTDDS_CALM_MIN_BUDGET_SAMPLE_MULTIPLIER", 0.25)
CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER: float = env_float(
    "FASTDDS_CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER", 1.0)
CALM_MAX_BUDGET_SAMPLE_MULTIPLIER: float = env_float(
    "FASTDDS_CALM_MAX_BUDGET_SAMPLE_MULTIPLIER", 2.0)
CALM_AI_B_SAMPLE_MULTIPLIER: float = env_float(
    "FASTDDS_CALM_AI_B_SAMPLE_MULTIPLIER", 0.25)
CALM_BUDGET_SCALING_MODE: str = os.environ.get(
    "FASTDDS_CALM_BUDGET_SCALING_MODE", "floor")
CALM_STORM_GAMMA: float = env_float("FASTDDS_CALM_STORM_GAMMA", 0.75)
CALM_STORM_RATE_GAMMA: float = env_float("FASTDDS_CALM_STORM_RATE_GAMMA", 0.875)
CALM_FIRST_FAILURE_GAMMA: float = env_float(
    "FASTDDS_CALM_FIRST_FAILURE_GAMMA", 0.80)
CALM_FIRST_FAILURE_RATE_GAMMA: float = env_float(
    "FASTDDS_CALM_FIRST_FAILURE_RATE_GAMMA", 0.90)
CALM_STORM_RATE_AI_MBPS: float = env_float("FASTDDS_CALM_STORM_RATE_AI_MBPS", 16.0)
CALM_MIN_RATE_OFFERED_MULTIPLIER: float = env_float(
    "FASTDDS_CALM_MIN_RATE_OFFERED_MULTIPLIER", 0.0)
CALM_INITIAL_RATE_OFFERED_MULTIPLIER: float = env_float(
    "FASTDDS_CALM_INITIAL_RATE_OFFERED_MULTIPLIER", 0.0)
CALM_MAX_RATE_OFFERED_MULTIPLIER: float = env_float(
    "FASTDDS_CALM_MAX_RATE_OFFERED_MULTIPLIER", 0.0)
CALM_AI_V_OFFERED_MULTIPLIER: float = env_float(
    "FASTDDS_CALM_AI_V_OFFERED_MULTIPLIER", 0.0)
CALM_OFFERED_RATE_EWMA_ALPHA: float = env_float(
    "FASTDDS_CALM_OFFERED_RATE_EWMA_ALPHA", 0.2)
CALM_PACING_DRAIN_GUARD: float = env_float("FASTDDS_CALM_PACING_DRAIN_GUARD", 1.10)
CALM_DETECTOR_RHO_BYTES: int = env_int(
    "FASTDDS_CALM_DETECTOR_RHO_BYTES", 4 * 1024 * 1024
)
CALM_DETECTOR_RHO_SAMPLE_MULTIPLIER: float = env_float(
    "FASTDDS_CALM_DETECTOR_RHO_SAMPLE_MULTIPLIER", 2.0)
CALM_DETECTOR_RETRY_COUNT: int = env_int("FASTDDS_CALM_DETECTOR_RETRY_COUNT", 2)
CALM_DETECTOR_RETRY_SIGNAL: str = os.getenv(
    "FASTDDS_CALM_DETECTOR_RETRY_SIGNAL", "rho_progress")
CALM_DETECTOR_FAILED_REPAIR_COUNT: int = env_int(
    "FASTDDS_CALM_DETECTOR_FAILED_REPAIR_COUNT", 2)
CALM_DETECTOR_GROWTH_ROUNDS: int = env_int("FASTDDS_CALM_DETECTOR_GROWTH_ROUNDS", 2)
CALM_DETECTOR_ACK_STALL_MS: float = env_float(
    "FASTDDS_CALM_DETECTOR_ACK_STALL_MS", 500.0
)
CALM_ACK_STALL_MODE: str = os.getenv("FASTDDS_CALM_ACK_STALL_MODE", "fixed")
CALM_STORM_FEEDBACK_MIN_AGE_MS: float = env_float(
    "FASTDDS_CALM_STORM_FEEDBACK_MIN_AGE_MS", 100.0
)
CALM_FEEDBACK_GUARD_MODE: str = os.getenv(
    "FASTDDS_CALM_FEEDBACK_GUARD_MODE", "fixed"
)

HERTZ_RANGE = list(range(10, 31, 10))

# 퍼블리시 피리어드 계산
PUBLISH_PERIODS: List[float] = [1.0 / hz for hz in HERTZ_RANGE]

# 동적으로 페이로드 사이즈와 퍼블리시 피리어드 계산 (기존 로직 제거)
# PAYLOAD_SIZES: List[int] = []
# PUBLISH_PERIODS: List[float] = []
#
# for hz in HERTZ_RANGE:
#     publish_period = 1.0 / hz
#     payload_size = TARGET_BANDWIDTH_BYTES_PER_SEC // hz
#     PAYLOAD_SIZES.append(payload_size)
#     PUBLISH_PERIODS.append(publish_period)


# 기존 설정들 (수정하지 않음)
LOSS_RATES:        List[int] = [0]
MAX_SAMPLES_LIST:  List[int] = [400]
# 퍼블리셔/구독자 소켓 버퍼를 각각 지정 (리스트 길이 1이면 동일 값으로 한 종류만 실행)
# 실행 인자 형식 호환용 값입니다. C++ 코드에서는 버퍼 튜닝에 사용하지 않습니다.
PUB_SOCKET_BUFFERS: List[int] = [0]
SUB_SOCKET_BUFFERS: List[int] = [0]



PAYLOAD_TYPES:     List[str] = ['image']#'image', 'point'

MAX_COUNT:         int       = 2000
REPEAT_EACH:       int       = 2

# Main traffic QoS. CALM/default reliability 실험의 본류 토픽입니다.
MAIN_TOPIC_PREFIX: str = "main"
MAIN_QOS_MODE:     str = "reliable"      # reliable | best_effort
MAIN_QOS_DEPTH:    int = 1               # best_effort/keep_last일 때 사용

# CASE D: 링크 단절 후 복구 실험. 꺼두면 평상시 송수신입니다.
CASE_D_ENABLED: bool = False
CASE_D_TRIGGER_AFTER: int = 200
CASE_D_TRIGGER_COUNTS: List[int] = env_int_list(
    "CALM_CASE_D_TRIGGER_COUNTS", [200, 1000]
)
CASE_D_DURATION_SECS_LIST: List[int] = env_int_list(
    "CALM_CASE_D_DURATION_SECS", [20, 40]
)

# CASE L: main 송수신 중 별도 topic의 background traffic으로 손실 spike를 유도합니다.
CASE_L_ENABLED: bool = False
CASE_L_TRIGGER_AFTER: int = 200
CASE_L_TRIGGER_COUNTS: List[int] = env_int_list(
    "CALM_CASE_L_TRIGGER_COUNTS", [200, 900, 1600]
)
BG_COUNTS_LIST: List[int] = env_int_list("CALM_BG_COUNTS_LIST", [3, 5])
BG_DURATION_SECS: float = env_float("CALM_BG_DURATION_SECS", 5.0)
BG_PAYLOAD_SIZES_KB: List[int] = env_int_list(
    "CALM_BG_PAYLOAD_SIZES_KB", [2048]
)
BG_HERTZ_RANGE: List[int] = env_int_list("CALM_BG_HERTZ_RANGE", [30])
BG_PAYLOAD_TYPES: List[str] = ["image"]
BG_QOS_MODE: str = "best_effort"         # best_effort | reliable
BG_QOS_DEPTH: int = 1                    # pure load generator이므로 기본 keep_last(1)
BG_MAX_COUNT: int = 100000
BG_FLOW_START_GAP_NS: int = 1            # BG publisher 사이의 의도적 시작 간격(실제 OS 스케줄링 정밀도는 더 큼)
CASE_TRIGGER_TIMEOUT_S: float = 300.0

# CASE A: Subscriber 준비가 끝난 뒤 Publisher egress에 고정 인공 손실을 적용합니다.
CASE_A_LOSS_RATES: List[int] = [1, 5, 10, 20]

# 선택한 DDS의 CALM 동작과 독립적으로 재전송 폭풍 관측 CSV를 기록합니다.
STORM_OBSERVER_ENABLED: bool = True
NETWORK_MONITOR_INTERVAL_S: float = 0.5
# 먼저 stock StatefulWriter의 폭풍 발생 여부를 규명합니다. CALM 비교 시 True로 바꿉니다.
CALM_ENABLED_FOR_STORM_EXPERIMENT: bool = os.getenv(
    "CALM_STORM_ENABLED", "0"
).strip().lower() in {"1", "true", "on", "yes"}

# 재전송 폭풍 특성화 순서: CASE D -> CASE L -> CASE A.
EXPERIMENT_SCENARIOS = [
    ("case_d", True, False, False),
    ("case_l", False, True, False),
    ("case_a", False, False, True),
]

RUN_SELECTED_ONLY: bool = os.getenv(
    "CALM_RUN_SELECTED_ONLY", "0"
).strip().lower() in {"1", "true", "on", "yes"}
SELECTED_IDX_LIST: List[int] = [
    int(value.strip())
    for value in os.getenv("CALM_SELECTED_IDXS", "3,7").split(",")
    if value.strip()
]

# DDS/RMW selection. CALM_DDS_VENDOR takes precedence over a pre-existing
# RMW_IMPLEMENTATION so one command controls both local and remote endpoints.
_RMW_BY_VENDOR = {
    "fastdds": "rmw_fastrtps_cpp",
    "cyclonedds": "rmw_cyclonedds_cpp",
}
_VENDOR_ALIASES = {
    "fast": "fastdds",
    "fastrtps": "fastdds",
    "fastdds": "fastdds",
    "cyclone": "cyclonedds",
    "cyclonedds": "cyclonedds",
}
_VENDOR_BY_RMW = {rmw: vendor for vendor, rmw in _RMW_BY_VENDOR.items()}
_requested_vendor = os.getenv("CALM_DDS_VENDOR", "").strip().lower()
_requested_rmw = os.getenv("RMW_IMPLEMENTATION", "").strip()
if _requested_vendor:
    DDS_VENDOR = _VENDOR_ALIASES.get(_requested_vendor, "")
    if not DDS_VENDOR:
        raise RuntimeError(
            f"Invalid CALM_DDS_VENDOR={_requested_vendor!r}; "
            "choose fastdds or cyclonedds"
        )
    RMW_IMPLEMENTATION = _RMW_BY_VENDOR[DDS_VENDOR]
elif _requested_rmw:
    if _requested_rmw not in _VENDOR_BY_RMW:
        raise RuntimeError(
            f"Unsupported RMW_IMPLEMENTATION={_requested_rmw!r}; "
            "choose rmw_fastrtps_cpp or rmw_cyclonedds_cpp"
        )
    RMW_IMPLEMENTATION = _requested_rmw
    DDS_VENDOR = _VENDOR_BY_RMW[RMW_IMPLEMENTATION]
else:
    DDS_VENDOR = "fastdds"
    RMW_IMPLEMENTATION = _RMW_BY_VENDOR[DDS_VENDOR]

IS_FASTDDS = DDS_VENDOR == "fastdds"
IS_CYCLONEDDS = DDS_VENDOR == "cyclonedds"
os.environ["RMW_IMPLEMENTATION"] = RMW_IMPLEMENTATION
if IS_CYCLONEDDS and CALM_ENABLED_FOR_STORM_EXPERIMENT:
    raise RuntimeError(
        "CALM_STORM_ENABLED=1 is unavailable for Cyclone DDS: CALM is currently "
        "implemented only in the modified Fast DDS writer"
    )
STORM_OBSERVER_ACTIVE = STORM_OBSERVER_ENABLED
print(f"[DDS] vendor={DDS_VENDOR} rmw={RMW_IMPLEMENTATION}")
# Configuration paths (로컬=퍼블리셔 → resource_pub, 원격=섭스크라이버 → resource_sub)
_SCRIPT_DIR:       str = os.path.dirname(os.path.abspath(__file__))
_SOURCE_PKG_ROOT:  str = os.path.dirname(_SCRIPT_DIR)
_INSTALL_PREFIX:   str = os.path.dirname(os.path.dirname(_SCRIPT_DIR))
_INSTALLED_SHARE:  str = os.path.join(_INSTALL_PREFIX, "share", "calm_pretest_yw")
_PKG_ROOT:         str = (
    _SOURCE_PKG_ROOT
    if os.path.isdir(os.path.join(_SOURCE_PKG_ROOT, "config"))
    else _INSTALLED_SHARE
)
SUB_XML:           str = os.path.join(_PKG_ROOT, "resource_pub", "subscriber_new.xml")
PUB_XML:           str = os.path.join(_PKG_ROOT, "resource_pub", "publisher_new.xml")
LOCAL_RESET_SH:    str = os.path.join(_PKG_ROOT, "resource_pub", "reset_sub.sh")
LOCAL_PKG_ROOT:    str = _PKG_ROOT  # optimized XML 찾기용 (large-data-optimization 상위)

# Select with CALM_NETWORK_MODE=ethernet or CALM_NETWORK_MODE=wifi.
NETWORK_CONFIGS = {
    "ethernet": {
        "local_ip": "192.168.50.1",
        "remote_ip": "192.168.50.2",
        "interface": "enp1s0",
        "fastdds_pub_profile": "ethernet_pub.xml",
        "fastdds_sub_profile": "ethernet_sub.xml",
        "cyclonedds_pub_profile": "cyclonedds_ethernet_pub.xml",
        "cyclonedds_sub_profile": "cyclonedds_ethernet_sub.xml",
    },
    "wifi": {
        "local_ip": "192.168.0.2",
        "remote_ip": "192.168.0.3",
        "interface": "wlp2s0",
        "fastdds_pub_profile": "wifi_pub.xml",
        "fastdds_sub_profile": "wifi_sub.xml",
        "cyclonedds_pub_profile": "cyclonedds_wifi_pub.xml",
        "cyclonedds_sub_profile": "cyclonedds_wifi_sub.xml",
    },
}
NETWORK_MODE: str = os.getenv("CALM_NETWORK_MODE", "ethernet").strip().lower()
if NETWORK_MODE not in NETWORK_CONFIGS:
    valid_modes = ", ".join(sorted(NETWORK_CONFIGS))
    raise RuntimeError(
        f"Invalid CALM_NETWORK_MODE={NETWORK_MODE!r}; choose one of: {valid_modes}"
    )
NETWORK_CONFIG = NETWORK_CONFIGS[NETWORK_MODE]

REMOTE_HOST:       str = os.getenv("CALM_REMOTE_HOST", NETWORK_CONFIG["remote_ip"])
REMOTE_USER:       str = "csilab"
REMOTE_WS_PATH:    str = "/home/csilab/ros2_ws"
REMOTE_PUB_XML:    str = f"{REMOTE_WS_PATH}/src/calm_pretest_yw/resource_sub/publisher_new.xml"
REMOTE_SUB_XML:    str = f"{REMOTE_WS_PATH}/src/calm_pretest_yw/resource_sub/subscriber_new.xml"
REMOTE_PKG_ROOT:   str = f"{REMOTE_WS_PATH}/src/calm_pretest_yw"
REMOTE_RESET_SH:   str = f"{REMOTE_WS_PATH}/src/calm_pretest_yw/resource_sub/reset_sub.sh"
LOCAL_DDS_XML:     str = os.path.join(
    _PKG_ROOT, "config", NETWORK_CONFIG[f"{DDS_VENDOR}_pub_profile"])
LOCAL_SUB_DDS_XML: str = os.path.join(
    _PKG_ROOT, "config", NETWORK_CONFIG[f"{DDS_VENDOR}_sub_profile"])
REMOTE_DDS_XML:    str = (
    f"{REMOTE_WS_PATH}/src/calm_pretest_yw/config/"
    f"{NETWORK_CONFIG[f'{DDS_VENDOR}_sub_profile']}"
)
REMOTE_RUNTIME_DDS_XML: str = (
    f"{REMOTE_RESULT_DIR}/.calm_runtime_profiles/"
    f"{NETWORK_CONFIG[f'{DDS_VENDOR}_sub_profile']}"
)
CALM_NET_INTERFACE: str = os.getenv(
    "CALM_NET_INTERFACE", NETWORK_CONFIG["interface"])
CALM_SUB_INACTIVE_TIMEOUT_S: float = env_float(
    "CALM_SUB_INACTIVE_TIMEOUT_S", 300.0)


def get_remote_sub_xml() -> str:
    """Return the active remote subscriber profile."""
    return globals().get("ACTIVE_REMOTE_DDS_XML", REMOTE_RUNTIME_DDS_XML)


def get_pub_xml() -> str:
    """Return the active local publisher profile."""
    return globals().get("ACTIVE_LOCAL_DDS_XML", LOCAL_DDS_XML)


# SSH password-based automation (consider SSH keys for production)
SSH_PASSWORD:      str = "csi123"
SSH_OPTS:          list = ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null"]


def prepare_network_mode() -> None:
    """Validate the selected link and deploy its Subscriber DDS profile."""
    local_ip = NETWORK_CONFIG["local_ip"]
    remote_ip = NETWORK_CONFIG["remote_ip"]

    for profile in (LOCAL_DDS_XML, LOCAL_SUB_DDS_XML):
        if not os.path.isfile(profile):
            raise RuntimeError(f"DDS profile does not exist: {profile}")

    local_rmw = subprocess.run(
        ["ros2", "pkg", "prefix", RMW_IMPLEMENTATION],
        capture_output=True, text=True, timeout=10, check=False
    )
    if local_rmw.returncode != 0:
        raise RuntimeError(
            f"Local ROS overlay does not provide {RMW_IMPLEMENTATION}; "
            "source /opt/ros/humble/setup.bash and ros2_ws/install/setup.bash"
        )

    local_address = subprocess.run(
        ["ip", "-o", "-4", "addr", "show", "dev", CALM_NET_INTERFACE],
        capture_output=True, text=True, check=False
    )
    if (
        local_address.returncode != 0 or
        f"inet {local_ip}/" not in local_address.stdout
    ):
        raise RuntimeError(
            f"{NETWORK_MODE} mode requires {local_ip} on {CALM_NET_INTERFACE}. "
            f"Current addresses: {local_address.stdout.strip() or '(none)'}"
        )

    remote_config_dir = os.path.dirname(REMOTE_RUNTIME_DDS_XML)
    remote_check_command = (
        f"mkdir -p {shlex.quote(remote_config_dir)} && "
        f"ip -o -4 addr show dev {shlex.quote(CALM_NET_INTERFACE)} "
        f"| grep -Fq {shlex.quote(f'inet {remote_ip}/')} && "
        f"source /opt/ros/humble/setup.bash && "
        f"source {shlex.quote(REMOTE_WS_PATH)}/install/setup.bash && "
        f"ros2 pkg prefix {shlex.quote(RMW_IMPLEMENTATION)} >/dev/null"
    )
    remote_check = subprocess.run(
        [
            "sshpass", "-p", SSH_PASSWORD, "ssh", *SSH_OPTS,
            f"{REMOTE_USER}@{REMOTE_HOST}", remote_check_command
        ],
        capture_output=True, text=True, timeout=20, check=False
    )
    if remote_check.returncode != 0:
        detail = remote_check.stderr.strip() or remote_check.stdout.strip()
        raise RuntimeError(
            f"{NETWORK_MODE} mode cannot verify {remote_ip} on remote "
            f"{CALM_NET_INTERFACE} via {REMOTE_USER}@{REMOTE_HOST}: "
            f"{detail or 'address/interface mismatch or SSH failure'}"
        )

    profile_copy = subprocess.run(
        [
            "sshpass", "-p", SSH_PASSWORD, "scp", *SSH_OPTS,
            LOCAL_SUB_DDS_XML,
            f"{REMOTE_USER}@{REMOTE_HOST}:{REMOTE_RUNTIME_DDS_XML}",
        ],
        capture_output=True, text=True, timeout=20, check=False
    )
    if profile_copy.returncode != 0:
        raise RuntimeError(
            f"Failed to deploy remote DDS profile {REMOTE_RUNTIME_DDS_XML}: "
            f"{profile_copy.stderr.strip()}"
        )

    print(
        f"[NET] dds={DDS_VENDOR} rmw={RMW_IMPLEMENTATION} "
        f"mode={NETWORK_MODE} interface={CALM_NET_INTERFACE} "
        f"publisher={local_ip} subscriber={remote_ip}"
    )
    print(f"[NET] publisher profile={LOCAL_DDS_XML}")
    print(f"[NET] subscriber profile={REMOTE_RUNTIME_DDS_XML}")


def restore_local_tc_baseline() -> None:
    """Restore the configured netem baseline, or the interface default qdisc."""
    permission_check = subprocess.run(
        ["sudo", "-n", "tc", "qdisc", "show", "dev", CALM_NET_INTERFACE],
        capture_output=True, text=True, timeout=5, check=False
    )
    if permission_check.returncode != 0:
        detail = permission_check.stderr.strip() or permission_check.stdout.strip()
        raise RuntimeError(
            "CASE A/D requires non-interactive sudo permission for tc: "
            f"{detail or 'sudo -n tc failed'}"
        )

    def env_float(name: str) -> float:
        try:
            return float(os.getenv(name, "0") or 0)
        except ValueError:
            return 0.0

    rate_mbit = max(0.0, env_float("CALM_NETEM_RATE_MBIT"))
    delay_ms = max(0.0, env_float("CALM_NETEM_DELAY_MS"))
    jitter_ms = max(0.0, env_float("CALM_NETEM_JITTER_MS"))
    loss_pct = min(100.0, max(0.0, env_float("CALM_NETEM_BASE_LOSS_PCT")))

    if rate_mbit <= 0.0 and delay_ms <= 0.0 and loss_pct <= 0.0:
        deleted = subprocess.run(
            ["sudo", "-n", "tc", "qdisc", "del", "dev", CALM_NET_INTERFACE, "root"],
            capture_output=True, text=True, timeout=5, check=False
        )
        current = subprocess.run(
            ["tc", "qdisc", "show", "dev", CALM_NET_INTERFACE],
            capture_output=True, text=True, timeout=5, check=False
        )
        if deleted.returncode != 0 and "netem" in current.stdout:
            detail = deleted.stderr.strip() or deleted.stdout.strip()
            raise RuntimeError(
                f"Failed to clear netem on {CALM_NET_INTERFACE}: {detail}"
            )
        print(f"[TC] baseline restored: interface={CALM_NET_INTERFACE} loss=0%")
        return

    command = [
        "sudo", "-n", "tc", "qdisc", "replace", "dev", CALM_NET_INTERFACE,
        "root", "netem", "limit", "10000"
    ]
    if delay_ms > 0.0:
        command.extend(["delay", f"{delay_ms}ms"])
        if jitter_ms > 0.0:
            command.extend([f"{jitter_ms}ms", "distribution", "normal"])
    if loss_pct > 0.0:
        command.extend(["loss", f"{loss_pct}%"])
    if rate_mbit > 0.0:
        command.extend(["rate", f"{rate_mbit}mbit"])

    restored = subprocess.run(
        command, capture_output=True, text=True, timeout=5, check=False
    )
    if restored.returncode != 0:
        detail = restored.stderr.strip() or restored.stdout.strip()
        raise RuntimeError(
            f"Failed to restore netem baseline on {CALM_NET_INTERFACE}: {detail}"
        )
    print(
        f"[TC] baseline restored: interface={CALM_NET_INTERFACE} "
        f"rate={rate_mbit}mbit delay={delay_ms}ms jitter={jitter_ms}ms "
        f"loss={loss_pct}%"
    )


def apply_local_tc_loss(loss_pct: float) -> None:
    """Apply an egress loss override without involving the DDS process."""
    loss_pct = min(100.0, max(0.0, float(loss_pct)))
    applied = subprocess.run(
        [
            "sudo", "-n", "tc", "qdisc", "replace", "dev",
            CALM_NET_INTERFACE, "root", "netem", "limit", "10000",
            "loss", f"{loss_pct}%",
        ],
        capture_output=True, text=True, timeout=5, check=False
    )
    if applied.returncode != 0:
        detail = applied.stderr.strip() or applied.stdout.strip()
        raise RuntimeError(
            f"Failed to apply {loss_pct}% loss on {CALM_NET_INTERFACE}: {detail}"
        )
    print(
        f"[TC] loss override applied: interface={CALM_NET_INTERFACE} "
        f"loss={loss_pct}%"
    )


# Paper-based DDS Optimization for Wireless Large Payload Transfer.
# By default only rules 1 and 2 are active: MTU-safe RTPS messages and
# heartbeatPeriod=1/(2r). Rule 3 (T-dependent history sizing) is opt-in.
USE_DDS_OPTIMIZER: bool = os.getenv(
    "DDS_WIRELESS_OPTIMIZER", "1"
).strip().lower() in {"1", "true", "on", "yes"}
DDS_OPT_HISTORY_CACHE_ENABLED: bool = os.getenv(
    "DDS_OPT_HISTORY_CACHE_ENABLED", "0"
).strip().lower() in {"1", "true", "on", "yes"}
LINK_THROUGHPUT_BPS = (
    env_int("DDS_OPT_LINK_THROUGHPUT_BPS", 240_000_000)
    if DDS_OPT_HISTORY_CACHE_ENABLED else None
)
LINK_UTILIZATION = (
    env_float("DDS_OPT_LINK_UTILIZATION", 0.6)
    if DDS_OPT_HISTORY_CACHE_ENABLED else None
)
DDS_OPT_MAX_MESSAGE_SIZE: int = env_int("DDS_OPT_MAX_MESSAGE_SIZE", 1472)
DDS_OPT_MAX_BLOCKING_TIME_SEC: int = env_int(
    "DDS_OPT_MAX_BLOCKING_TIME_SEC", 1000)
ACTIVE_LOCAL_DDS_XML: str = LOCAL_DDS_XML
ACTIVE_REMOTE_DDS_XML: str = REMOTE_RUNTIME_DDS_XML


def dds_optimizer_label() -> str:
    if not USE_DDS_OPTIMIZER:
        return "stock"
    if IS_CYCLONEDDS:
        return "ddsopt12_analog"
    return "ddsopt123" if DDS_OPT_HISTORY_CACHE_ENABLED else "ddsopt12"


def _deploy_remote_profile(local_profile: str) -> None:
    copied = subprocess.run(
        [
            "sshpass", "-p", SSH_PASSWORD, "scp", *SSH_OPTS,
            local_profile,
            f"{REMOTE_USER}@{REMOTE_HOST}:{REMOTE_RUNTIME_DDS_XML}",
        ],
        capture_output=True, text=True, timeout=20, check=False)
    if copied.returncode != 0:
        raise RuntimeError(
            f"Failed to deploy DDS optimizer profile: {copied.stderr.strip()}")


def prepare_dds_optimizer_profiles(idx: int, payload_bytes: int, publish_rate_hz: float):
    """Generate and activate the per-experiment publisher/subscriber XML pair."""
    global ACTIVE_LOCAL_DDS_XML, ACTIVE_REMOTE_DDS_XML

    if not USE_DDS_OPTIMIZER:
        ACTIVE_LOCAL_DDS_XML = LOCAL_DDS_XML
        ACTIVE_REMOTE_DDS_XML = REMOTE_RUNTIME_DDS_XML
        _deploy_remote_profile(LOCAL_SUB_DDS_XML)
        return None

    parameters = compute_parameters(
        publish_rate_hz,
        payload_bytes,
        LINK_THROUGHPUT_BPS,
        LINK_UTILIZATION,
        DDS_OPT_MAX_MESSAGE_SIZE,
        history_cache_enabled=DDS_OPT_HISTORY_CACHE_ENABLED)
    profile_dir = os.path.join(RESULT_DIR, "dds_optimizer_profiles")
    os.makedirs(profile_dir, exist_ok=True)
    stem = f"idx{idx}_p{payload_bytes}_h{publish_rate_hz:g}"
    publisher_profile = os.path.join(profile_dir, f"{stem}_pub.xml")
    subscriber_profile = os.path.join(profile_dir, f"{stem}_sub.xml")
    if IS_CYCLONEDDS:
        generate_cyclonedds_profiles(
            parameters,
            NETWORK_CONFIG["local_ip"],
            NETWORK_CONFIG["remote_ip"],
            publisher_profile,
            subscriber_profile)
    else:
        generate_profiles(
            parameters,
            NETWORK_CONFIG["local_ip"],
            NETWORK_CONFIG["remote_ip"],
            publisher_profile,
            subscriber_profile,
            DDS_OPT_MAX_BLOCKING_TIME_SEC)
    _deploy_remote_profile(subscriber_profile)
    ACTIVE_LOCAL_DDS_XML = publisher_profile
    ACTIVE_REMOTE_DDS_XML = REMOTE_RUNTIME_DDS_XML

    if parameters.history_cache_enabled:
        assert parameters.allocated_link_bps is not None
        assert parameters.offered_to_allocated_ratio is not None
        state = "VALID" if parameters.offered_to_allocated_ratio <= 1.0 else "OVERLOAD"
        history_summary = (
            f"historyCache={parameters.history_cache_samples} samples "
            f"allocated={parameters.allocated_link_bps / 1e6:.3f}Mbps "
            f"ratio={parameters.offered_to_allocated_ratio:.3f}")
    else:
        state = "NOT_EVALUATED"
        history_summary = "historyCache=default linkCapacity=unused"
    print(
        f"[DDS_OPT] vendor={DDS_VENDOR} mode={dds_optimizer_label()} state={state} "
        f"maxMessageSize={parameters.max_message_size} "
        f"heartbeatPeriod={parameters.heartbeat_period_ns}ns "
        f"{history_summary} "
        f"offered={parameters.offered_load_bps / 1e6:.3f}Mbps")
    return parameters

def generate_param_list() -> list:
    tuples = []
    idx = 1
    # 소켓 버퍼별로 한 바퀴 → 각 버퍼 크기에서 (페이로드×헤르츠) 대역폭 범위 전부 실행
    for scenario_name, case_d_enabled, case_l_enabled, case_a_enabled in EXPERIMENT_SCENARIOS:
        case_d_durations = [CASE_D_DURATION_SECS_LIST[0]] if case_d_enabled else [0]
        case_l_bg_counts = BG_COUNTS_LIST if case_l_enabled else [0]
        case_a_losses = CASE_A_LOSS_RATES if case_a_enabled else [0]
        for beta, gamma, alpha, q_min in product(
            CALM_BETA_LIST, CALM_GAMMA_LIST, CALM_ALPHA_LIST, CALM_Q_MIN_LIST
        ):
            for lr, ms, pub_sb, sub_sb, pt, bs, bg_count, case_a_loss_pct in product(
                LOSS_RATES, MAX_SAMPLES_LIST, PUB_SOCKET_BUFFERS,
                SUB_SOCKET_BUFFERS, PAYLOAD_TYPES, case_d_durations,
                case_l_bg_counts, case_a_losses
            ):
                for payload_bytes in PAYLOAD_SIZES_BYTES:
                    for hz in HERTZ_RANGE:
                        publish_period = 1.0 / hz
                        for _ in range(REPEAT_EACH):
                            tuples.append((
                                payload_bytes, lr, ms, idx, pub_sb, sub_sb, pt, bs,
                                publish_period, hz, scenario_name, case_d_enabled,
                                case_l_enabled, case_a_enabled, beta, gamma, alpha,
                                q_min, bg_count, case_a_loss_pct
                            ))
                            idx += 1
    return tuples



def kill_remote_sub(force: bool = False) -> None:
    pkill = "pkill -9 -f 'dds_sub'" if force else "pkill -f 'dds_sub'"
    subprocess.run(["sshpass", "-p", SSH_PASSWORD, "ssh", *SSH_OPTS, f"{REMOTE_USER}@{REMOTE_HOST}", pkill],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def kill_local_pub(force: bool = False) -> None:
    if force:
        # 강제 종료: SIGKILL 사용
        subprocess.run(["pkill", "-9", "-f", "dds_pub"], 
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # 추가로 ros2run 프로세스도 종료
        subprocess.run(["pkill", "-9", "-f", "ros2 run calm_pretest_yw dds_pub"], 
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Fast DDS와 Cyclone DDS는 dds_pub 프로세스 안에서 동작하는
        # 라이브러리입니다. 벤더 이름으로 pkill하면 automation을 실행한
        # 상위 셸까지 오인 종료할 수 있으므로 실행 파일만 정리합니다.
    else:
        # 정상 종료: SIGTERM 사용
        subprocess.run(["pkill", "-f", "dds_pub"], 
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def calm_result_suffix(beta: float, gamma: float, alpha: float, q_min: float,
                       scenario_name: str, case_a_loss_pct: int = 0) -> str:
    scenario_label = {
        "case_d": "d",
        "case_l": "l",
        "case_a": f"a{case_a_loss_pct}",
    }.get(scenario_name, "normal")
    return f"beta_{beta}_gamma_{gamma}_alpha_{alpha}_qmin_{q_min}_{scenario_label}"


def fetch_remote_sub_csv(loss: int, idx: int, payload: int, hz: int, max_samples: int,
                         socket_buffer: int, burst_secs: int, payload_type: str,
                         topic_prefix: str = MAIN_TOPIC_PREFIX, qos_mode: str = MAIN_QOS_MODE,
                         max_count: int = MAX_COUNT, result_suffix: str = "") -> None:
    """완료된 Subscriber 개별 CSV를 원격 노트북에서 로컬 결과 폴더로 복사합니다."""
    filename = (
        f"sub_loss{float(loss):.1f}_idx{idx}_payload{payload}_hz{hz}_count{max_count}"
        f"_samples{max_samples}_sock{socket_buffer}_burst{burst_secs}_{payload_type}.csv"
    )
    base_prefixed_filename = (
        f"sub_loss{float(loss):.1f}_idx{idx}_payload{payload}_hz{hz}_count{max_count}"
        f"_samples{max_samples}_sock{socket_buffer}_burst{burst_secs}"
        f"_topic{topic_prefix or 'default'}_qos{qos_mode or 'reliable'}_{payload_type}"
    )
    prefixed_filename = f"{base_prefixed_filename}{'_' + result_suffix if result_suffix else ''}.csv"
    sanitized_result_suffix = re.sub(r"[^A-Za-z0-9_]", "_", result_suffix)
    sanitized_prefixed_filename = (
        f"{base_prefixed_filename}"
        f"{'_' + sanitized_result_suffix if sanitized_result_suffix else ''}.csv"
    )
    fallback_prefixed_filename = f"{base_prefixed_filename}.csv"
    local_file = os.path.join(RESULT_DIR, prefixed_filename)

    try:
        candidates = [
            sanitized_prefixed_filename, prefixed_filename,
            fallback_prefixed_filename, filename
        ]
        last_error = ""
        for remote_filename in dict.fromkeys(candidates):
            remote_file = f"{REMOTE_RESULT_DIR}/{remote_filename}"
            result = subprocess.run(
                ["sshpass", "-p", SSH_PASSWORD, "scp", *SSH_OPTS,
                 f"{REMOTE_USER}@{REMOTE_HOST}:{remote_file}", local_file],
                capture_output=True, text=True, timeout=30
            )
            if result.returncode == 0:
                debug_log(f"Fetched remote Subscriber CSV: {remote_filename} -> {prefixed_filename}")
                return
            last_error = result.stderr.strip()
        debug_log(f"Failed to fetch Subscriber CSV candidates: {candidates} ({last_error})")
    except Exception as e:
        debug_log(f"Error fetching Subscriber CSV: {e}")

def start_zenoh_router_local() -> subprocess.Popen:
    """로컬 Zenoh 라우터 시작 (발행 호스트: 192.168.50.22:7447)"""
    router_env = os.environ.copy()
    router_env["ZENOH_CONFIG_OVERRIDE"] = 'connect/endpoints=["tcp/192.168.50.22:7447"]'
    router_cmd = ["ros2", "run", "rmw_zenoh_cpp", "rmw_zenohd"]
    p_router = subprocess.Popen(router_cmd, env=router_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    debug_log(f"Local Zenoh router started (PID: {p_router.pid})")
    time.sleep(2.0)  # 라우터 시작 대기
    return p_router

def start_zenoh_router_remote() -> subprocess.Popen:
    """원격 Zenoh 라우터 시작 (구독 호스트: 192.168.50.189:7447)"""
    router_cmd = [
        "sshpass", "-p", SSH_PASSWORD, "ssh", "-T", *SSH_OPTS, f"{REMOTE_USER}@{REMOTE_HOST}",
        f"bash -lc '"
        f"source /opt/ros/humble/setup.bash && "
        f"export ZENOH_CONFIG_OVERRIDE=\"connect/endpoints=[\\\"tcp/192.168.50.189:7447\\\"]\" && "
        f"ros2 run rmw_zenoh_cpp rmw_zenohd'"
    ]
    p_router = subprocess.Popen(router_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    debug_log(f"Remote Zenoh router started (PID: {p_router.pid})")
    time.sleep(2.0)  # 라우터 시작 대기
    return p_router

def kill_zenoh_router_local(p_router: subprocess.Popen = None) -> None:
    """로컬 Zenoh 라우터 종료"""
    if p_router:
        try:
            p_router.terminate()
            p_router.wait(timeout=5)
            debug_log("Local Zenoh router terminated")
        except subprocess.TimeoutExpired:
            p_router.kill()
            debug_log("Local Zenoh router killed")
        except Exception as e:
            debug_log(f"Error terminating local Zenoh router: {e}")
    # 백업: pkill로도 종료 시도
    subprocess.run(["pkill", "-f", "rmw_zenohd"], 
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def kill_zenoh_router_remote(p_router: subprocess.Popen = None) -> None:
    """원격 Zenoh 라우터 종료"""
    if p_router:
        try:
            p_router.terminate()
            p_router.wait(timeout=5)
            debug_log("Remote Zenoh router terminated")
        except subprocess.TimeoutExpired:
            p_router.kill()
            debug_log("Remote Zenoh router killed")
        except Exception as e:
            debug_log(f"Error terminating remote Zenoh router: {e}")
    # 백업: 원격에서 pkill로도 종료 시도
    subprocess.run(["sshpass", "-p", SSH_PASSWORD, "ssh", *SSH_OPTS, f"{REMOTE_USER}@{REMOTE_HOST}", "pkill -f rmw_zenohd"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build_local_pub_cmd(loss: int, idx: int, payload: int, max_samples: int, socket_buffer: int,
                        payload_type: str, publish_period: float, burst_secs: int = 0, hz: int = 0,
                        max_count: int = MAX_COUNT, topic_prefix: str = MAIN_TOPIC_PREFIX,
                        qos_mode: str = MAIN_QOS_MODE, qos_depth: int = MAIN_QOS_DEPTH,
                        case_d_enabled: bool = False, trigger_after: int = CASE_D_TRIGGER_AFTER) -> list:
    return [
        "ros2", "run", "calm_pretest_yw", "dds_pub",
        str(loss), str(idx), str(payload), str(max_count), str(max_samples), str(socket_buffer),
        payload_type, str(publish_period), str(burst_secs), str(hz), topic_prefix, qos_mode,
        str(qos_depth), "1" if case_d_enabled else "0", str(trigger_after)
    ]


def get_rmw_xml_env_var(xml_path: str) -> dict:
    """Return the profile environment for the selected RMW."""
    env = {}
    if IS_FASTDDS:
        env["FASTRTPS_DEFAULT_PROFILES_FILE"] = xml_path
    elif IS_CYCLONEDDS:
        env["CYCLONEDDS_URI"] = f"file://{xml_path}"
    return env


def get_remote_rmw_profile_shell(xml_path: str) -> str:
    """Return shell commands that activate only the selected remote profile."""
    if IS_FASTDDS:
        return (
            f"export FASTRTPS_DEFAULT_PROFILES_FILE={shlex.quote(xml_path)} && "
            "unset FASTDDS_DEFAULT_PROFILES_FILE CYCLONEDDS_URI "
            "RMW_FASTRTPS_USE_QOS_FROM_XML"
        )
    return (
        f"export CYCLONEDDS_URI={shlex.quote(f'file://{xml_path}')} && "
        "unset FASTRTPS_DEFAULT_PROFILES_FILE FASTDDS_DEFAULT_PROFILES_FILE "
        "RMW_FASTRTPS_USE_QOS_FROM_XML"
    )

def build_remote_sub_cmd(loss: int, idx: int, payload: int, max_samples: int, socket_buffer: int, burst_secs: int, payload_type: str,
                         T_bps: int = None, publish_rate_hz: float = None, max_count: int = MAX_COUNT,
                         topic_prefix: str = MAIN_TOPIC_PREFIX, qos_mode: str = MAIN_QOS_MODE,
                         qos_depth: int = MAIN_QOS_DEPTH, result_suffix: str = "",
                         progress_thresholds: List[int] = None) -> str:
    progress_csv = ",".join(str(value) for value in (progress_thresholds or []))
    profile_setup = get_remote_rmw_profile_shell(ACTIVE_REMOTE_DDS_XML)
    return (
        f"bash -lc '"
        f"source /opt/ros/humble/setup.bash && "
        f"cd {REMOTE_WS_PATH} && source install/setup.bash && "
        f"export RMW_IMPLEMENTATION={RMW_IMPLEMENTATION} && "
        f"export LOOPBACK_TEST_PKG_ROOT={REMOTE_PKG_ROOT} && "
        f"export CALM_RESULT_DIR={REMOTE_RESULT_DIR} && "
        f"{profile_setup} && "
        f"export CALM_NET_INTERFACE={CALM_NET_INTERFACE} && "
        f"export CALM_SUB_INACTIVE_TIMEOUT_S={CALM_SUB_INACTIVE_TIMEOUT_S} && "
        f"export CALM_PROGRESS_THRESHOLDS={progress_csv} && "
        f"export ROS_LOCALHOST_ONLY=0 && "
        f"export ROS_DOMAIN_ID=25 && "
        f"ros2 run calm_pretest_yw dds_sub {loss} {idx} {payload} {max_count} {max_samples} {socket_buffer} "
        f"{burst_secs} {payload_type} {int(round(publish_rate_hz or 0))} {topic_prefix} {qos_mode} {qos_depth}"
        f"{' ' + result_suffix if result_suffix else ''}'"
    )


def terminate_process(p: subprocess.Popen, name: str, grace_s: float = 1.0) -> None:
    if p is None or p.poll() is not None:
        return
    try:
        debug_log(f"Terminating {name} pid={p.pid}")
        p.terminate()
        p.wait(timeout=grace_s)
    except subprocess.TimeoutExpired:
        debug_log(f"Killing {name} pid={p.pid}")
        try:
            p.kill()
        except Exception:
            pass
    except Exception as e:
        debug_log(f"Error terminating {name}: {e}")


def kill_remote_by_topic_prefix(executable: str, topic_prefix: str) -> None:
    pattern = f"{executable} .* {topic_prefix} "
    cmd = f"pkill -f '{pattern}'"
    subprocess.run(["sshpass", "-p", SSH_PASSWORD, "ssh", *SSH_OPTS, f"{REMOTE_USER}@{REMOTE_HOST}", cmd],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def kill_local_by_topic_prefix(executable: str, topic_prefix: str) -> None:
    pattern = f"{executable} .* {topic_prefix} "
    subprocess.run(["pkill", "-f", pattern], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def wait_for_receive_trigger(pub_output: list, p_pub: subprocess.Popen,
                             p_sub: subprocess.Popen, trigger_count: int) -> bool:
    token = f"[SUB_PROGRESS] recv_count={trigger_count}"
    start = time.time()
    while (time.time() - start) < CASE_TRIGGER_TIMEOUT_S:
        if any(token in line for line in pub_output):
            debug_log(f"Receive-count trigger observed: {trigger_count}")
            return True
        if p_pub.poll() is not None or p_sub.poll() is not None:
            debug_log(f"Main endpoint ended before receive-count trigger {trigger_count}")
            return False
        time.sleep(0.1)
    debug_log(f"Receive-count trigger {trigger_count} timed out after {CASE_TRIGGER_TIMEOUT_S:.1f}s")
    return False


def start_case_l_background(idx: int, loss: int, max_samples: int, pub_socket_buffer: int,
                            sub_socket_buffer: int, pub_output: list, p_pub: subprocess.Popen,
                            p_sub: subprocess.Popen, bg_count: int, event_log_path: str,
                            case_l_enabled: bool = CASE_L_ENABLED) -> str:
    if not case_l_enabled:
        return "off"

    completed_bursts = []
    os.makedirs(os.path.dirname(event_log_path), exist_ok=True)
    with open(event_log_path, "w", newline="") as event_file:
        event_writer = csv.writer(event_file)
        event_writer.writerow([
            "time_ns", "scenario", "action", "trigger_recv_count",
            "bg_count", "duration_s", "bg_payload_kb", "bg_hz"
        ])

        for cycle, trigger_count in enumerate(CASE_L_TRIGGER_COUNTS, start=1):
            if not wait_for_receive_trigger(pub_output, p_pub, p_sub, trigger_count):
                completed_bursts.append(f"{trigger_count}:trigger_missed")
                break

            bg_processes = []
            bg_threads = []
            summaries = []
            bg_specs = []
            for bg_i in range(1, bg_count + 1):
                bg_prefix = f"bg_c{cycle}_{bg_i}"
                bg_payload_kb = BG_PAYLOAD_SIZES_KB[(bg_i - 1) % len(BG_PAYLOAD_SIZES_KB)]
                bg_payload = bg_payload_kb * 1024
                bg_hz = BG_HERTZ_RANGE[(bg_i - 1) % len(BG_HERTZ_RANGE)]
                bg_period = 1.0 / float(bg_hz)
                bg_type = BG_PAYLOAD_TYPES[(bg_i - 1) % len(BG_PAYLOAD_TYPES)]
                bg_idx = idx * 10000 + cycle * 100 + bg_i
                summaries.append(f"{bg_prefix}:{bg_payload_kb}KB@{bg_hz}Hz")
                bg_specs.append((bg_i, bg_prefix, bg_payload, bg_hz, bg_period, bg_type, bg_idx))

            for bg_i, bg_prefix, bg_payload, bg_hz, _bg_period, bg_type, bg_idx in bg_specs:
                remote_cmd = [
                    "sshpass", "-p", SSH_PASSWORD, "ssh", "-T", *SSH_OPTS,
                    f"{REMOTE_USER}@{REMOTE_HOST}",
                    build_remote_sub_cmd(
                        loss, bg_idx, bg_payload, max_samples, sub_socket_buffer, 0, bg_type,
                        publish_rate_hz=bg_hz, max_count=BG_MAX_COUNT, topic_prefix=bg_prefix,
                        qos_mode=BG_QOS_MODE, qos_depth=BG_QOS_DEPTH
                    )
                ]
                p_bg_sub = subprocess.Popen(
                    remote_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
                )
                bg_sub_output = []
                t_bg_sub = threading.Thread(
                    target=stream_output, args=(f'BG{cycle}-{bg_i}-SUB', p_bg_sub.stdout, bg_sub_output)
                )
                t_bg_sub.start()
                bg_processes.append(("remote_sub", bg_prefix, p_bg_sub))
                bg_threads.append(t_bg_sub)

            for spec_i, (bg_i, bg_prefix, bg_payload, bg_hz, bg_period, bg_type, bg_idx) in enumerate(bg_specs):
                bg_pub_cmd = build_local_pub_cmd(
                    loss, bg_idx, bg_payload, max_samples, pub_socket_buffer, bg_type,
                    bg_period, 0, bg_hz, max_count=BG_MAX_COUNT, topic_prefix=bg_prefix,
                    qos_mode=BG_QOS_MODE, qos_depth=BG_QOS_DEPTH, case_d_enabled=False,
                    trigger_after=trigger_count
                )
                bg_env = os.environ.copy()
                for name in (
                    "FASTRTPS_DEFAULT_PROFILES_FILE", "FASTDDS_DEFAULT_PROFILES_FILE",
                    "CYCLONEDDS_URI", "FASTDDS_CALM_LOG_DIR", "FASTDDS_STORM_LOG_FILE",
                    "CYCLONEDDS_STORM_LOG_FILE"
                ):
                    bg_env.pop(name, None)
                bg_env["LOOPBACK_TEST_PKG_ROOT"] = LOCAL_PKG_ROOT
                bg_env["CALM_RESULT_DIR"] = RESULT_DIR
                bg_env.update(get_rmw_xml_env_var(ACTIVE_LOCAL_DDS_XML))
                bg_env["CALM_NET_INTERFACE"] = CALM_NET_INTERFACE
                bg_env["ROS_DOMAIN_ID"] = "25"
                bg_env["RMW_IMPLEMENTATION"] = RMW_IMPLEMENTATION
                bg_env["ROS_LOCALHOST_ONLY"] = "0"
                bg_env["FASTDDS_CALM_ENABLED"] = "0"
                bg_env["FASTDDS_STORM_OBSERVER_ENABLED"] = "0"
                bg_env["CYCLONEDDS_STORM_OBSERVER_ENABLED"] = "0"
                p_bg_pub = subprocess.Popen(
                    bg_pub_cmd, env=bg_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    stdin=subprocess.PIPE, text=True, bufsize=1, universal_newlines=True
                )
                bg_pub_output = []
                t_bg_pub = threading.Thread(
                    target=stream_output, args=(f'BG{cycle}-{bg_i}-PUB', p_bg_pub.stdout, bg_pub_output)
                )
                t_bg_pub.start()
                bg_processes.append(("local_pub", bg_prefix, p_bg_pub))
                bg_threads.append(t_bg_pub)
                if spec_i + 1 < len(bg_specs) and BG_FLOW_START_GAP_NS > 0:
                    time.sleep(BG_FLOW_START_GAP_NS / 1_000_000_000.0)

            event_writer.writerow([
                time.time_ns(), "case_l", "start", trigger_count, bg_count,
                BG_DURATION_SECS, ";".join(map(str, BG_PAYLOAD_SIZES_KB)),
                ";".join(map(str, BG_HERTZ_RANGE))
            ])
            event_file.flush()
            print(
                f"[CASE_L_EVENT] action=start recv_count={trigger_count} cycle={cycle} "
                f"bg_count={bg_count} duration_s={BG_DURATION_SECS:.1f}"
            )
            time.sleep(BG_DURATION_SECS)

            for role, prefix, proc in bg_processes:
                terminate_process(proc, f"CASE_L {role} {prefix}")
            for role, prefix, _ in bg_processes:
                if role == "remote_sub":
                    kill_remote_by_topic_prefix("dds_sub", prefix)
                else:
                    kill_local_by_topic_prefix("dds_pub", prefix)
            for thread in bg_threads:
                thread.join(timeout=1)

            event_writer.writerow([
                time.time_ns(), "case_l", "end", trigger_count, bg_count,
                BG_DURATION_SECS, ";".join(map(str, BG_PAYLOAD_SIZES_KB)),
                ";".join(map(str, BG_HERTZ_RANGE))
            ])
            event_file.flush()
            print(f"[CASE_L_EVENT] action=end recv_count={trigger_count} cycle={cycle}")
            completed_bursts.append(f"{trigger_count}:{bg_count}flows")

    return ",".join(completed_bursts)
# REMOTE_WS_PATH: "/home/csilab/ros2_ws"
def run_local_reset() -> None:
    try:
        debug_log(f"Executing local reset script: {LOCAL_RESET_SH}")
        subprocess.run(["bash", LOCAL_RESET_SH], timeout=10)
        debug_log("Local reset script executed successfully")
    except subprocess.TimeoutExpired:
        debug_log("local reset script timeout")
    except Exception as e:
        debug_log(f"Error in local reset: {e}")
# LOCAL_RESET_SH: resource_pub/reset_sub.sh (패키지 기준)
def run_remote_reset() -> None:
    # 원격 스크립트가 존재/실행 가능한지 확인 후 실행
    check_cmd = (
        f"bash -lc 'if [ -x {REMOTE_RESET_SH} ]; then echo READY; else echo MISSING; fi'"
    )
    try:
        debug_log(f"Checking remote reset script: {REMOTE_RESET_SH}")
        res = subprocess.run(["sshpass", "-p", SSH_PASSWORD, "ssh", *SSH_OPTS, f"{REMOTE_USER}@{REMOTE_HOST}", check_cmd], capture_output=True, text=True, timeout=10)
        if res.returncode != 0:
            debug_log(f"remote reset check failed rc={res.returncode} stderr={res.stderr.strip() if res.stderr else ''}")
            return
        state = res.stdout.strip()
        debug_log(f"Remote reset script state: {state}")
        if state == "READY":
            debug_log(f"Executing remote reset script: {REMOTE_RESET_SH}")
            subprocess.run(["sshpass", "-p", SSH_PASSWORD, "ssh", *SSH_OPTS, f"{REMOTE_USER}@{REMOTE_HOST}", f"bash {REMOTE_RESET_SH}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            debug_log("Remote reset script executed successfully")
        else:
            debug_log(f"remote reset script not found at {REMOTE_RESET_SH}, performing manual cleanup...")
            # 스크립트가 없어도 원격 호스트에서 DDS 리소스 정리 수행
            try:
                # ros2 daemon 재시작 및 공유 메모리 정리
                cleanup_cmd = (
                    f"bash -lc '"
                    f"source /opt/ros/humble/setup.bash 2>/dev/null || true; "
                    f"ros2 daemon stop > /dev/null 2>&1; "
                    f"sleep 1; "
                    f"rm -rf /dev/shm/fastdds* > /dev/null 2>&1; "
                    f"rm -rf /dev/shm/cyclonedds* > /dev/null 2>&1; "
                    f"sleep 1; "
                    f"ros2 daemon start > /dev/null 2>&1; "
                    f"echo DONE'"
                )
                res_cleanup = subprocess.run(
                    ["sshpass", "-p", SSH_PASSWORD, "ssh", *SSH_OPTS, f"{REMOTE_USER}@{REMOTE_HOST}", cleanup_cmd],
                    capture_output=True, text=True, timeout=15
                )
                if res_cleanup.returncode == 0:
                    debug_log("Remote DDS cleanup completed successfully")
                else:
                    debug_log(f"Remote DDS cleanup failed: {res_cleanup.stderr.strip() if res_cleanup.stderr else 'unknown error'}")
            except subprocess.TimeoutExpired:
                debug_log("Remote DDS cleanup timeout")
            except Exception as e:
                debug_log(f"Error in remote DDS cleanup: {e}")
    except subprocess.TimeoutExpired:
        debug_log("remote reset timeout")
    except Exception as e:
        debug_log(f"Error in remote reset: {e}")

def reset_system_udp_buffers(pub_socket_buffer: int = None, sub_socket_buffer: int = None) -> None:
    """시스템 UDP 버퍼: rx(수신)=sub_socket_buffer, tx(송신)=pub_socket_buffer. None이면 기본값 유지."""
    default_rmem_default = 212992
    default_wmem_default = 212992
    default_rmem_max = 212922
    default_wmem_max = 212992

    rmem_default = sub_socket_buffer if sub_socket_buffer is not None else default_rmem_default
    rmem_max = max(rmem_default, default_rmem_max)
    wmem_default = pub_socket_buffer if pub_socket_buffer is not None else default_wmem_default
    wmem_max = max(wmem_default, default_wmem_max)

    # sudo 권한을 미리 획득하여 타임아웃을 늘림
    try:
        subprocess.run(["sudo", "-v"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        debug_log("sudo credentials refreshed successfully")
    except subprocess.CalledProcessError:
        debug_log("Failed to refresh sudo credentials")
        return

    commands = [
        f"sudo sysctl -w net.core.rmem_default={rmem_default}",
        f"sudo sysctl -w net.core.wmem_default={wmem_default}",
        f"sudo sysctl -w net.core.rmem_max={rmem_max}",
        f"sudo sysctl -w net.core.wmem_max={wmem_max}",
    ]
    
    for cmd in commands:
        try:
            subprocess.run(cmd.split(), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            pass  # 권한 오류나 기타 문제 시 무시

def _parse_udp_stats_text(text: str) -> dict:
    """netstat -s 출력에서 UDP 통계를 파싱합니다."""
    lines = text.split('\n')
    udp_section = False
    stats = {
        'packet_receive_errors': 0,
        'receive_buffer_errors': 0,
        'send_buffer_errors': 0,
        'packets_received': 0,
        'packets_sent': 0,
    }
    for raw in lines:
        line = raw.strip()
        if not line:
            if udp_section:
                break
            continue
        if 'Udp:' in line or line == 'Udp':
            udp_section = True
            debug_log("Found UDP section in netstat output")
            continue
        if not udp_section:
            continue
        try:
            if 'packet receive errors' in line:
                stats['packet_receive_errors'] = int(line.split()[0])
            elif 'receive buffer errors' in line:
                stats['receive_buffer_errors'] = int(line.split()[0])
            elif 'send buffer errors' in line:
                stats['send_buffer_errors'] = int(line.split()[0])
            elif 'packets received' in line:
                stats['packets_received'] = int(line.split()[0])
            elif 'packets sent' in line:
                stats['packets_sent'] = int(line.split()[0])
        except Exception:
            pass
    if not udp_section:
        debug_log("UDP section not found in netstat output. Printing head:")
        head = '\n'.join(lines[:20])
        debug_log(head)
    return stats

def _read_udp_stats_from_proc() -> dict:
    """/proc/net/snmp에서 UDP 카운터를 읽습니다 (netstat 대체, root 불필요)."""
    try:
        with open("/proc/net/snmp", "r") as f:
            lines = f.read().strip().splitlines()
        headers = None
        values = None
        for i in range(len(lines)):
            if lines[i].startswith("Udp:"):
                parts = lines[i].split()[1:]
                headers = parts
                if i + 1 < len(lines) and lines[i+1].startswith("Udp:"):
                    values = lines[i+1].split()[1:]
                break
        if not headers or not values or len(headers) != len(values):
            debug_log("/proc/net/snmp UDP section not found or malformed")
            return {}
        data = dict(zip(headers, [int(v) for v in values]))
        mapped = {
            'packet_receive_errors': data.get('InErrors', 0),
            'receive_buffer_errors': data.get('RcvbufErrors', 0),
            'send_buffer_errors': data.get('SndbufErrors', 0),
            'packets_received': data.get('InDatagrams', 0),
            'packets_sent': data.get('OutDatagrams', 0),
        }
        return mapped
    except Exception as e:
        debug_log(f"Failed reading /proc/net/snmp: {e}")
        return {}


def get_udp_stats() -> dict:
    """현재 UDP 통계를 가져옵니다."""
    try:
        # 1) /proc 기반 우선 사용 (보다 안정적이고 권한 필요 없음)
        proc_stats = _read_udp_stats_from_proc()
        if proc_stats:
            return proc_stats
        # 2) netstat 대체 경로
        result = subprocess.run(['netstat', '-s'], capture_output=True, text=True)
        if result.returncode != 0:
            debug_log(f"netstat -s failed rc={result.returncode} stderr={result.stderr.strip() if result.stderr else ''}")
            return {}
        debug_log("netstat -s succeeded; parsing UDP section")
        return _parse_udp_stats_text(result.stdout)
    except Exception:
        return {}


def _read_interface_counter(name: str) -> int:
    path = f"/sys/class/net/{CALM_NET_INTERFACE}/statistics/{name}"
    try:
        with open(path, "r") as counter_file:
            return int(counter_file.read().strip())
    except (OSError, ValueError):
        return 0


def _read_qdisc_stats() -> dict:
    stats = {
        "qdisc_sent_bytes": 0,
        "qdisc_sent_packets": 0,
        "qdisc_dropped": 0,
        "qdisc_overlimits": 0,
        "qdisc_requeues": 0,
        "qdisc_backlog_bytes": 0,
        "qdisc_backlog_packets": 0,
    }
    try:
        result = subprocess.run(
            ["tc", "-s", "qdisc", "show", "dev", CALM_NET_INTERFACE],
            capture_output=True, text=True, timeout=2, check=False
        )
        sent_matches = re.findall(
            r"Sent\s+(\d+)\s+bytes\s+(\d+)\s+pkt.*?"
            r"\(dropped\s+(\d+),\s+overlimits\s+(\d+)\s+requeues\s+(\d+)\)",
            result.stdout
        )
        backlog_matches = re.findall(r"backlog\s+(\d+)b\s+(\d+)p", result.stdout)
        if sent_matches:
            stats["qdisc_sent_bytes"] = sum(int(match[0]) for match in sent_matches)
            stats["qdisc_sent_packets"] = sum(int(match[1]) for match in sent_matches)
            stats["qdisc_dropped"] = sum(int(match[2]) for match in sent_matches)
            stats["qdisc_overlimits"] = sum(int(match[3]) for match in sent_matches)
            stats["qdisc_requeues"] = sum(int(match[4]) for match in sent_matches)
        if backlog_matches:
            stats["qdisc_backlog_bytes"] = sum(int(match[0]) for match in backlog_matches)
            stats["qdisc_backlog_packets"] = sum(int(match[1]) for match in backlog_matches)
    except (OSError, subprocess.SubprocessError):
        pass
    return stats


def network_monitor_worker(output_path: str, stop_event: threading.Event) -> None:
    fields = [
        "time_ns", "elapsed_s", "interface",
        "tx_bytes", "tx_packets", "tx_dropped", "tx_errors",
        "rx_bytes", "rx_packets", "rx_dropped", "rx_errors",
        "udp_in_errors", "udp_rcvbuf_errors", "udp_sndbuf_errors",
        "udp_packets_received", "udp_packets_sent",
        "qdisc_sent_bytes", "qdisc_sent_packets", "qdisc_dropped",
        "qdisc_overlimits", "qdisc_requeues",
        "qdisc_backlog_bytes", "qdisc_backlog_packets"
    ]
    start = time.monotonic()
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", newline="") as monitor_file:
        writer = csv.DictWriter(monitor_file, fieldnames=fields)
        writer.writeheader()
        while not stop_event.is_set():
            udp = get_udp_stats()
            qdisc = _read_qdisc_stats()
            row = {
                "time_ns": time.time_ns(),
                "elapsed_s": f"{time.monotonic() - start:.6f}",
                "interface": CALM_NET_INTERFACE,
                "tx_bytes": _read_interface_counter("tx_bytes"),
                "tx_packets": _read_interface_counter("tx_packets"),
                "tx_dropped": _read_interface_counter("tx_dropped"),
                "tx_errors": _read_interface_counter("tx_errors"),
                "rx_bytes": _read_interface_counter("rx_bytes"),
                "rx_packets": _read_interface_counter("rx_packets"),
                "rx_dropped": _read_interface_counter("rx_dropped"),
                "rx_errors": _read_interface_counter("rx_errors"),
                "udp_in_errors": udp.get("packet_receive_errors", 0),
                "udp_rcvbuf_errors": udp.get("receive_buffer_errors", 0),
                "udp_sndbuf_errors": udp.get("send_buffer_errors", 0),
                "udp_packets_received": udp.get("packets_received", 0),
                "udp_packets_sent": udp.get("packets_sent", 0),
                **qdisc,
            }
            writer.writerow(row)
            monitor_file.flush()
            stop_event.wait(NETWORK_MONITOR_INTERVAL_S)


def start_network_monitor(output_path: str) -> Tuple[threading.Event, threading.Thread]:
    stop_event = threading.Event()
    thread = threading.Thread(
        target=network_monitor_worker, args=(output_path, stop_event), daemon=True
    )
    thread.start()
    return stop_event, thread


def stop_network_monitor(stop_event: threading.Event, thread: threading.Thread) -> None:
    stop_event.set()
    thread.join(timeout=max(2.0, NETWORK_MONITOR_INTERVAL_S * 3.0))


def _append_case_d_event(pub_output: list, **values) -> None:
    fields = " ".join(f"{key}={value}" for key, value in values.items())
    line = f"[CASE_D_EVENT] {fields}"
    pub_output.append(line)
    print(f"[CASE_D_SUPERVISOR] {fields}")

def case_d_supervisor_worker(
    pub_output: list,
    p_pub: subprocess.Popen,
    p_sub: subprocess.Popen,
    stop_event: threading.Event,
    state: dict,
) -> None:
    """Inject CASE D outages independently of the potentially blocked DDS writer."""
    scan_index = 0
    latest_recv_count = 0
    try:
        for trigger, duration_s in zip(
            CASE_D_TRIGGER_COUNTS, CASE_D_DURATION_SECS_LIST
        ):
            while latest_recv_count < trigger and not stop_event.is_set():
                snapshot = pub_output[scan_index:]
                scan_index += len(snapshot)
                for line in snapshot:
                    match = re.search(r"\[SUB_PROGRESS\]\s+recv_count=(\d+)", line)
                    if match:
                        latest_recv_count = max(
                            latest_recv_count, int(match.group(1))
                        )
                if latest_recv_count >= trigger:
                    break
                if p_pub.poll() is not None or p_sub.poll() is not None:
                    state["incomplete"] = True
                    return
                stop_event.wait(0.05)

            if stop_event.is_set():
                state["incomplete"] = True
                return

            apply_local_tc_loss(100.0)
            start_ns = time.time_ns()
            _append_case_d_event(
                pub_output,
                time_ns=start_ns,
                scenario="case_d",
                action="start",
                recv_count=latest_recv_count,
                trigger=trigger,
                duration_s=duration_s,
                controller="automation",
                applied=1,
            )

            interrupted = stop_event.wait(float(duration_s))
            restore_local_tc_baseline()
            end_ns = time.time_ns()
            _append_case_d_event(
                pub_output,
                time_ns=end_ns,
                scenario="case_d",
                action="end",
                recv_count=latest_recv_count,
                trigger=trigger,
                duration_s=duration_s,
                elapsed_s=f"{(end_ns - start_ns) / 1e9:.6f}",
                controller="automation",
                restored_loss=0,
                interrupted=1 if interrupted else 0,
            )
            state["completed_outages"] = state.get("completed_outages", 0) + 1
            if interrupted:
                state["incomplete"] = True
                return
    except Exception as exc:
        state["error"] = exc
    finally:
        try:
            restore_local_tc_baseline()
        except Exception as exc:
            state.setdefault("error", exc)


def start_case_d_supervisor(
    pub_output: list,
    p_pub: subprocess.Popen,
    p_sub: subprocess.Popen,
) -> Tuple[threading.Event, threading.Thread, dict]:
    stop_event = threading.Event()
    state = {"completed_outages": 0, "incomplete": False, "error": None}
    thread = threading.Thread(
        target=case_d_supervisor_worker,
        args=(pub_output, p_pub, p_sub, stop_event, state),
        daemon=True,
    )
    thread.start()
    return stop_event, thread, state


def stop_case_d_supervisor(
    stop_event: threading.Event,
    thread: threading.Thread,
    state: dict,
) -> None:
    stop_event.set()
    thread.join(timeout=10.0)
    restore_local_tc_baseline()
    if thread.is_alive():
        raise RuntimeError("CASE D supervisor did not stop within 10 seconds")
    if state.get("error") is not None:
        raise RuntimeError(f"CASE D supervisor failed: {state['error']}")


def write_case_d_event_log(output_path: str, pub_output: list) -> None:
    fields = [
        "time_ns", "scenario", "action", "recv_count", "trigger",
        "duration_s", "elapsed_s", "controller", "applied", "interrupted",
        "next_trigger_index", "restored_loss"
    ]
    with open(output_path, "w", newline="") as event_file:
        writer = csv.DictWriter(event_file, fieldnames=fields)
        writer.writeheader()
        for line in pub_output:
            if "[CASE_D_EVENT]" not in line:
                continue
            values = dict(re.findall(r"([A-Za-z_]+)=([^\s]+)", line))
            writer.writerow({
                "time_ns": values.get("time_ns", ""),
                "scenario": "case_d",
                "action": values.get("action", ""),
                "recv_count": values.get("recv_count", ""),
                "trigger": values.get("trigger", ""),
                "duration_s": values.get("duration_s", ""),
                "elapsed_s": values.get("elapsed_s", ""),
                "controller": values.get("controller", "publisher"),
                "applied": values.get("applied", ""),
                "interrupted": values.get("interrupted", ""),
                "next_trigger_index": values.get("next_trigger_index", ""),
                "restored_loss": values.get("restored_loss", ""),
            })


def write_case_a_event_log(
    output_path: str,
    pub_output: list,
    case_a_loss_pct: int,
    baseline_restored: bool,
) -> None:
    fields = [
        "time_ns", "scenario", "action", "loss_pct", "applied",
        "restored_loss", "restored"
    ]
    with open(output_path, "w", newline="") as event_file:
        writer = csv.DictWriter(event_file, fieldnames=fields)
        writer.writeheader()
        end_event_found = False
        for line in pub_output:
            if "[CASE_A_EVENT]" not in line:
                continue
            values = dict(re.findall(r"([A-Za-z_]+)=([^\s]+)", line))
            end_event_found = end_event_found or values.get("action") == "end"
            writer.writerow({
                "time_ns": values.get("time_ns", ""),
                "scenario": "case_a",
                "action": values.get("action", ""),
                "loss_pct": values.get("loss_pct", ""),
                "applied": values.get("applied", ""),
                "restored_loss": values.get("restored_loss", ""),
                "restored": values.get("restored", ""),
            })
        if not end_event_found:
            writer.writerow({
                "time_ns": time.time_ns(),
                "scenario": "case_a",
                "action": "end",
                "loss_pct": case_a_loss_pct,
                "applied": "",
                "restored_loss": os.getenv("CALM_NETEM_BASE_LOSS_PCT", "0"),
                "restored": 1 if baseline_restored else 0,
            })


def _parse_socket_buf_lines(output_list, role: str):
    """Parse 'local=IP:PORT SO_SNDBUF=N' from PUB/SUB output. Returns list of (local_addr_port, so_sndbuf)."""
    result = []
    trigger = "[PUB] Sockets after publish done" if role == "PUB" else "[SUB] Sockets after receive done"
    collecting = False
    for line in output_list:
        if trigger in line:
            collecting = True
            continue
        if not collecting or "local=" not in line or "SO_SNDBUF=" not in line:
            if collecting and ("[PUB_STATS]" in line or "[SUB_STATS]" in line or (role in line and "Sockets" in line and "done" not in line)):
                break
            continue
        try:
            rest = line.split("local=", 1)[1]
            local_part = rest.split(" SO_SNDBUF=")[0].strip()
            buf_part = rest.split("SO_SNDBUF=")[1].strip().split()[0]
            if buf_part.isdigit():
                result.append((local_part, int(buf_part)))
        except (IndexError, ValueError):
            pass
    return result


def print_unified_log(idx, payload, loss, payload_type, burst_secs, max_samples, pub_socket_buffer, sub_socket_buffer,
                      publish_period, max_count, csv_writer, csvfile, sub_output, pub_output, udp_stats_before,
                      udp_stats_after, exit_mode: str = "normal", case_l_summary: str = "off",
                      scenario_name: str = "normal", case_d_enabled: bool = False,
                      case_l_enabled: bool = False, case_a_enabled: bool = False,
                      case_a_loss_pct: int = 0, calm_beta: float = 0.8,
                      calm_gamma: float = 0.5, calm_alpha: float = 1.0,
                      calm_q_min: float = 0.05, bg_count: int = 0,
                      optimizer_parameters=None):
    """통합 로그 출력 및 CSV 저장. exit_mode: normal / sub_force / pub_force / sub_force,pub_force"""
    try:
        debug_log(f"print_unified_log: Starting - Parse SUB lines: {len(sub_output)} | PUB lines: {len(pub_output)}")
        # Subscriber 통계 파싱
        debug_log("Parsing subscriber statistics...")
        hz_sub = 0
        std_delay = 0
        avg_delay = 0
        hz_pub = 0
        recv_count = 0  # 받은 메시지 수 추가
        
        for line in sub_output:
            if "[SUB_STATS]" in line:
                try:
                    if "hz_sub:" in line:
                        val = line.split("hz_sub:")[1].strip()
                        hz_sub = float(val) if val != 'nan' else 0.0
                    elif "avg_delay:" in line:
                        val = line.split("avg_delay:")[1].strip()
                        avg_delay = float(val) if val != 'nan' else 0.0
                    elif "std_delay:" in line:
                        val = line.split("std_delay:")[1].strip()
                        std_delay = float(val) if val != 'nan' else 0.0
                    elif "hz_pub:" in line:
                        val = line.split("hz_pub:")[1].strip()
                        hz_pub = float(val) if val != 'nan' else 0.0
                    elif "recv_count:" in line:  # recv_count 파싱 추가
                        val = line.split("recv_count:")[1].strip()
                        recv_count = int(val) if val.isdigit() else 0
                except (ValueError, IndexError):
                    continue
        
        # Publisher 통계 파싱
        debug_log("Parsing publisher statistics...")
        publish_rate = 0.0
        avg_interval = 0
        std_interval = 0
        avg_pub_time = 0
        std_pub_time = 0
        
        for line in pub_output:
            if "[PUB_STATS]" in line:
                try:
                    if "publish_rate:" in line:
                        val = line.split("publish_rate:")[1].strip()
                        publish_rate = float(val) if val != 'nan' else 0.0
                    if "avg_interval:" in line:
                        val = line.split("avg_interval:")[1].strip()
                        avg_interval = float(val) 
                    if "std_interval:" in line:
                        val = line.split("std_interval:")[1].strip()
                        std_interval = float(val) 
                    if "avg_pub_time:" in line:
                        val = line.split("avg_pub_time:")[1].strip()
                        avg_pub_time = float(val) 
                    if "std_pub_time:" in line:
                        val = line.split("std_pub_time:")[1].strip()
                        std_pub_time = float(val) 
                except (ValueError, IndexError):
                    continue
        
        # UDP 통계 차이 계산
        debug_log("Calculating UDP statistics...")
        udp_packet_receive_errors = udp_stats_after.get('packet_receive_errors', 0) - udp_stats_before.get('packet_receive_errors', 0)
        udp_receive_buffer_errors = udp_stats_after.get('receive_buffer_errors', 0) - udp_stats_before.get('receive_buffer_errors', 0)
        udp_send_buffer_errors = udp_stats_after.get('send_buffer_errors', 0) - udp_stats_before.get('send_buffer_errors', 0)
        udp_packets_received = udp_stats_after.get('packets_received', 0) - udp_stats_before.get('packets_received', 0)
        udp_packets_sent = udp_stats_after.get('packets_sent', 0) - udp_stats_before.get('packets_sent', 0)
        if udp_packets_received == 0 and udp_packets_sent == 0:
            debug_log(f"UDP delta zeros. before={udp_stats_before} after={udp_stats_after}")

        # PUB/SUB 소켓 버퍼 파싱 (local=ip:port, SO_SNDBUF)
        pub_sockets = _parse_socket_buf_lines(pub_output, "PUB")
        sub_sockets = _parse_socket_buf_lines(sub_output, "SUB")
        pub_sockets_str = ";".join(f"{a}={b}" for a, b in pub_sockets)
        sub_sockets_str = ";".join(f"{a}={b}" for a, b in sub_sockets)

        # Zenoh 라우터 오류 감지
        zenoh_router_error = ""
        if RMW_IMPLEMENTATION == "rmw_zenoh_cpp":
            error_msg = "Unable to push non droppable network message"
            pub_error = any(error_msg in line for line in pub_output)
            sub_error = any(error_msg in line for line in sub_output)
            if pub_error and sub_error:
                zenoh_router_error = "라우터오류:pub+sub"
            elif pub_error:
                zenoh_router_error = "라우터오류:pub"
            elif sub_error:
                zenoh_router_error = "라우터오류:sub"

        # 요청된 형식으로 로그 출력
        debug_log("Printing statistics to console...")
        print("=" * 60)
        print(f"idx={idx} | dds={DDS_VENDOR} rmw={RMW_IMPLEMENTATION} | payload={payload}  loss={loss}%  max_samples={max_samples} pub_sock={pub_socket_buffer} sub_sock={sub_socket_buffer} payload_type={payload_type} burst_secs={burst_secs} publish_period={publish_period} mode: local")
        print(
            f"scenario={scenario_name} | CASE_D={'on' if case_d_enabled else 'off'} "
            f"triggers={CASE_D_TRIGGER_COUNTS} durations={CASE_D_DURATION_SECS_LIST} | "
            f"CASE_L={'on' if case_l_enabled else 'off'} "
            f"triggers={CASE_L_TRIGGER_COUNTS} bg_count={bg_count} bg={case_l_summary} | "
            f"CASE_A={'on' if case_a_enabled else 'off'} loss={case_a_loss_pct}%"
        )
        print(
            f"CALM enabled={CALM_ENABLED_FOR_STORM_EXPERIMENT} "
            f"beta={calm_beta} gamma={calm_gamma} alpha={calm_alpha} q_min={calm_q_min}"
        )
        if optimizer_parameters is not None:
            history_summary = (
                f"historyCache={optimizer_parameters.history_cache_samples} "
                f"load_ratio={optimizer_parameters.offered_to_allocated_ratio:.3f}"
                if optimizer_parameters.history_cache_enabled else
                "historyCache=default linkCapacity=unused"
            )
            print(
                f"DDS optimizer={dds_optimizer_label()} "
                f"maxMessageSize={optimizer_parameters.max_message_size} "
                f"heartbeatPeriod={optimizer_parameters.heartbeat_period_ns}ns "
                f"{history_summary}"
            )
        print(f"loss: {loss}% msgs: {max_count} received: {recv_count} sub hz: {hz_sub:.3f} (std: {std_delay:.3f}), e2e delay: {avg_delay:.3f} ms (std: {std_delay:.3f}), pub hz: {hz_pub:.3f} (std: {std_delay:.3f}) pub dt: {avg_interval:.3f} ms (std: {std_interval:.3f}), pub call delay: {avg_pub_time:.3f} ms (std: {std_pub_time:.3f})")
        print(f"UDP stats: recv_errors={udp_packet_receive_errors}, recv_buf_errors={udp_receive_buffer_errors}, send_buf_errors={udp_send_buffer_errors}, packets_recv={udp_packets_received}, packets_sent={udp_packets_sent}")
        print(f"exit_mode: {exit_mode}")
        
        # CSV에 저장 및 즉시 flush
        debug_log("Writing to CSV...")
        csv_writer.writerow([
            idx, payload, loss, payload_type, burst_secs, publish_period,
            hz_sub, std_delay, avg_delay, hz_pub, publish_rate, recv_count,
            avg_interval, std_interval, avg_pub_time, std_pub_time,
            udp_packet_receive_errors, udp_receive_buffer_errors, udp_send_buffer_errors,
            udp_packets_received, udp_packets_sent, zenoh_router_error,
            pub_sockets_str, sub_sockets_str,
            exit_mode,
            DDS_VENDOR, RMW_IMPLEMENTATION,
            MAIN_TOPIC_PREFIX, MAIN_QOS_MODE, MAIN_QOS_DEPTH,
            "on" if case_d_enabled else "off", ";".join(map(str, CASE_D_TRIGGER_COUNTS)),
            ";".join(map(str, CASE_D_DURATION_SECS_LIST)) if case_d_enabled else "",
            "on" if case_l_enabled else "off", ";".join(map(str, CASE_L_TRIGGER_COUNTS)),
            bg_count if case_l_enabled else 0,
            ";".join(str(v) for v in BG_PAYLOAD_SIZES_KB) if case_l_enabled else "",
            ";".join(str(v) for v in BG_HERTZ_RANGE) if case_l_enabled else "",
            BG_DURATION_SECS if case_l_enabled else 0,
            BG_QOS_MODE if case_l_enabled else "",
            BG_QOS_DEPTH if case_l_enabled else "",
            case_l_summary,
            "on" if case_a_enabled else "off",
            case_a_loss_pct if case_a_enabled else 0,
            scenario_name, "on" if CALM_ENABLED_FOR_STORM_EXPERIMENT else "off",
            calm_beta, calm_gamma, calm_alpha, calm_q_min,
            "on" if optimizer_parameters is not None else "off",
            dds_optimizer_label() if optimizer_parameters else "stock",
            optimizer_parameters.max_message_size if optimizer_parameters else "",
            optimizer_parameters.heartbeat_period_ns if optimizer_parameters else "",
            "on" if optimizer_parameters and optimizer_parameters.history_cache_enabled else "off",
            optimizer_parameters.history_cache_samples if optimizer_parameters else "",
            optimizer_parameters.link_throughput_bps if optimizer_parameters else "",
            optimizer_parameters.link_utilization if optimizer_parameters else "",
            optimizer_parameters.offered_load_bps if optimizer_parameters else "",
            optimizer_parameters.allocated_link_bps if optimizer_parameters else "",
            optimizer_parameters.offered_to_allocated_ratio if optimizer_parameters else "",
        ])
        debug_log("Flushing CSV file...")
        csvfile.flush()  # 즉시 디스크에 저장
        debug_log("print_unified_log: Completed successfully")
        
    except Exception as e:
        debug_log(f"Error in print_unified_log: {e}")
        import traceback
        debug_log(traceback.format_exc())


def main() -> None:
    global MAX_COUNT
    prepare_network_mode()
    restore_local_tc_baseline()

    # 통합 결과 CSV 파일 생성
    import csv
    os.makedirs(RESULT_DIR, exist_ok=True)
    result_fields = [
        'idx', 'payload', 'loss', 'payload_type', 'burst_secs', 'publish_period',
        'sub_hz', 'sub_std', 'e2e_delay', 'pub_hz', 'publish_rate', 'recv_count',
        'pub_dt', 'pub_dt_std', 'pub_call_delay', 'pub_call_delay_std',
        'udp_recv_errors', 'udp_recv_buf_errors', 'udp_send_buf_errors',
        'udp_packets_recv', 'udp_packets_sent', 'zenoh_router_error',
        'pub_sockets', 'sub_sockets', 'exit_mode',
        'dds_vendor', 'rmw_implementation',
        'main_topic_prefix', 'main_qos_mode', 'main_qos_depth',
        'case_d_enabled', 'case_d_trigger_counts', 'case_d_duration_secs',
        'case_l_enabled', 'case_l_trigger_counts',
        'bg_count', 'bg_payload_sizes_kb', 'bg_hz_range',
        'bg_duration_secs', 'bg_qos_mode', 'bg_qos_depth', 'case_l_summary',
        'case_a_enabled', 'case_a_loss_pct',
        'scenario_name', 'calm_enabled', 'calm_beta', 'calm_gamma',
        'calm_alpha', 'calm_q_min',
        'dds_optimizer_enabled', 'dds_optimizer_mode',
        'dds_opt_max_message_size', 'dds_opt_heartbeat_period_ns',
        'dds_opt_history_cache_enabled', 'dds_opt_history_cache_samples',
        'dds_opt_link_throughput_bps', 'dds_opt_link_utilization',
        'dds_opt_offered_load_bps', 'dds_opt_allocated_link_bps',
        'dds_opt_offered_to_allocated_ratio'
    ]
    result_csv_path = os.path.join(RESULT_DIR, "storm_experiment_results.csv")
    if os.path.isfile(result_csv_path):
        with open(result_csv_path, newline='') as existing_stream:
            existing_header = next(csv.reader(existing_stream), [])
        if existing_header != result_fields:
            result_csv_path = os.path.join(
                RESULT_DIR, f"storm_experiment_results_{dds_optimizer_label()}.csv")

    file_exists = os.path.exists(result_csv_path)
    
    # CSV 파일을 열고 헤더 작성 (기존 파일이 없을 때만)
    csvfile = open(result_csv_path, 'a', newline='')
    csv_writer = csv.writer(csvfile)
    
    # 기존 파일이 없을 때만 헤더 작성
    if not file_exists:
        csv_writer.writerow(result_fields)
        csvfile.flush()  # 헤더 즉시 저장
    
    # Zenoh 라우터 프로세스 (필요시에만 사용)
    p_zenoh_router_local = None
    p_zenoh_router_remote = None
    zenoh_routers_started = False
    
    try:
        for (
            payload, loss, max_samples, idx, pub_socket_buffer, sub_socket_buffer,
            payload_type, burst_secs, publish_period, hz, scenario_name,
            case_d_enabled, case_l_enabled, case_a_enabled, calm_beta,
            calm_gamma, calm_alpha, calm_q_min, bg_count, case_a_loss_pct
        ) in generate_param_list():
            if RUN_SELECTED_ONLY and idx not in SELECTED_IDX_LIST:
                continue
            # Default 실험과 동일하게 각 케이스마다 고정된 메시지 수를 전송합니다.
            payload_kb = payload // 1024
            hz_float = 1.0 / publish_period

            # Zenoh일 때 매 세션마다 라우터 시작
            if RMW_IMPLEMENTATION == "rmw_zenoh_cpp":
                debug_log("Starting Zenoh routers (this session)...")
                p_zenoh_router_local = start_zenoh_router_local()
                p_zenoh_router_remote = start_zenoh_router_remote()
                zenoh_routers_started = True
                debug_log("Zenoh routers started")

            # 테스트 세션 정보 출력
            payload_kb = payload // 1024
            hz_float = 1.0 / publish_period
            print(f"🚀 테스트 시작 - idx={idx} | 페이로드={payload_kb}KB | 헤르츠={hz_float:.1f}Hz | 타입={payload_type}")
            print(f"📊 설정: loss={loss}% | samples={max_samples} | pub_sock={pub_socket_buffer} sub_sock={sub_socket_buffer} | case_d_secs={burst_secs}")
            print(
                f"🧪 scenario={scenario_name} | CASE_D={'on' if case_d_enabled else 'off'} "
                f"| CASE_L={'on' if case_l_enabled else 'off'} bg_count={bg_count} "
                f"| CASE_A={'on' if case_a_enabled else 'off'} loss={case_a_loss_pct}% "
                f"| main_topic={MAIN_TOPIC_PREFIX} | main_qos={MAIN_QOS_MODE}"
            )
            print(f"🧬 CALM beta={calm_beta} gamma={calm_gamma} alpha={calm_alpha} q_min={calm_q_min}")
            print(f"⏱️  퍼블리시 피리어드: {publish_period:.6f}s | 예상 대역폭: {(payload_kb * hz_float * 8) / 1024:.1f}Mbps")
            theoretical_duration = MAX_COUNT * publish_period
            print(f"⏰ 이론적 실행 시간: {theoretical_duration:.1f}초 ({theoretical_duration/60:.1f}분) | 메시지 수: {MAX_COUNT}개")
            print("=" * 80)

            # CASE D/A가 비정상 종료되었더라도 다음 실험은 항상 설정된 baseline에서 시작합니다.
            restore_local_tc_baseline()
            optimizer_parameters = prepare_dds_optimizer_profiles(idx, payload, hz_float)
            time.sleep(3)
            # UDP 통계 수집 (테스트 전)
            udp_stats_before = get_udp_stats()

            # 시스템 UDP 버퍼는 변경하지 않고 OS 기본값을 사용합니다.

            # 출력 수집을 위한 리스트
            sub_output = []
            pub_output = []
            

                
    
            # Remote mode: subscriber on remote host, publisher locally
            
            # 1) Start remote subscriber (T, r 전달 → 원격 DDS_Optimizer용)
            result_suffix = calm_result_suffix(
                calm_beta, calm_gamma, calm_alpha, calm_q_min, scenario_name,
                case_a_loss_pct
            )
            result_suffix = f"{DDS_VENDOR}_{result_suffix}"
            if USE_DDS_OPTIMIZER:
                result_suffix += f"_{dds_optimizer_label()}"
            progress_thresholds = (
                CASE_D_TRIGGER_COUNTS if case_d_enabled else
                CASE_L_TRIGGER_COUNTS if case_l_enabled else []
            )
            mode_label = (
                "calm" if CALM_ENABLED_FOR_STORM_EXPERIMENT else "default"
            )
            experiment_tag = (
                f"idx{idx}_p{payload}_h{hz}_{DDS_VENDOR}_{mode_label}_{scenario_name}"
                f"_{dds_optimizer_label()}"
                f"{f'_bg{bg_count}' if case_l_enabled else ''}"
                f"{f'_loss{case_a_loss_pct}pct' if case_a_enabled else ''}"
            )
            event_log_path = os.path.join(
                RESULT_DIR, f"disturbance_events_{experiment_tag}.csv"
            )
            network_log_path = os.path.join(
                RESULT_DIR, f"network_storm_{experiment_tag}.csv"
            )
            storm_log_path = os.path.join(
                RESULT_DIR, f"{DDS_VENDOR}_storm_{experiment_tag}.csv"
            )
            remote_sub_cmd = [
                "sshpass", "-p", SSH_PASSWORD, "ssh", "-T", *SSH_OPTS, f"{REMOTE_USER}@{REMOTE_HOST}",
                build_remote_sub_cmd(loss, idx, payload, max_samples, sub_socket_buffer, burst_secs, payload_type,
                                     T_bps=LINK_THROUGHPUT_BPS, publish_rate_hz=hz, max_count=MAX_COUNT,
                                     topic_prefix=MAIN_TOPIC_PREFIX, qos_mode=MAIN_QOS_MODE,
                                     qos_depth=MAIN_QOS_DEPTH, result_suffix=result_suffix,
                                     progress_thresholds=progress_thresholds)
            ]
            p_sub = subprocess.Popen(remote_sub_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            
            # Subscriber 스레드 즉시 시작
            t_sub = threading.Thread(target=stream_output, args=('SUB', p_sub.stdout, sub_output))
            t_sub.start()
            
            # 2) Start local publisher
            time.sleep(2.0)
            pub_cmd = build_local_pub_cmd(
                loss, idx, payload, max_samples, pub_socket_buffer, payload_type,
                publish_period, burst_secs, hz, max_count=MAX_COUNT,
                topic_prefix=MAIN_TOPIC_PREFIX, qos_mode=MAIN_QOS_MODE,
                # CASE D tc control runs in the automation supervisor so a
                # blocking DDS write cannot starve the restoration timer.
                qos_depth=MAIN_QOS_DEPTH, case_d_enabled=False,
                trigger_after=CASE_D_TRIGGER_AFTER if case_d_enabled else CASE_L_TRIGGER_AFTER
            )
            pub_env = os.environ.copy()
            pub_env.pop("FASTRTPS_DEFAULT_PROFILES_FILE", None)
            pub_env.pop("FASTDDS_DEFAULT_PROFILES_FILE", None)
            pub_env.pop("CYCLONEDDS_URI", None)
            pub_env.pop("FASTDDS_RTPS_MAX_MESSAGE_SIZE", None)
            pub_env["LOOPBACK_TEST_PKG_ROOT"] = LOCAL_PKG_ROOT
            pub_env["CALM_RESULT_DIR"] = RESULT_DIR

            # Do not let exported CALM tuning from an earlier run leak into a
            # default-DDS control run. CALM OFF retains only the explicit gate.
            for env_name in tuple(pub_env):
                if env_name.startswith("FASTDDS_CALM_"):
                    pub_env.pop(env_name, None)
            pub_env["FASTDDS_CALM_ENABLED"] = (
                "1" if CALM_ENABLED_FOR_STORM_EXPERIMENT else "0"
            )
            if CALM_ENABLED_FOR_STORM_EXPERIMENT:
                pub_env.update({
                    "FASTDDS_CALM_LOG_DIR": RESULT_DIR,
                    "FASTDDS_CALM_BETA": str(calm_beta),
                    "FASTDDS_CALM_GAMMA": str(calm_gamma),
                    "FASTDDS_CALM_ALPHA": str(calm_alpha),
                    "FASTDDS_CALM_Q_MIN": str(calm_q_min),
                    "FASTDDS_CALM_CONTROLLER": CALM_CONTROLLER,
                    "FASTDDS_CALM_PACING_MS": str(CALM_PACING_MS),
                    "FASTDDS_CALM4_K_DEC": str(CALM4_K_DEC),
                    "FASTDDS_CALM4_K_INC": str(CALM4_K_INC),
                    "FASTDDS_CALM_FRAGMENT_BYTES_PER_PERIOD": str(
                        CALM_FRAGMENT_BYTES_PER_PERIOD
                    ),
                    "FASTDDS_CALM_FRAGMENT_PERIOD_MS": str(CALM_FRAGMENT_PERIOD_MS),
                    "FASTDDS_CALM_MAX_RTPS_MESSAGE_SIZE": str(CALM_MAX_RTPS_MESSAGE_SIZE),
                    "FASTDDS_CALM_MAX_BUDGET_BYTES": str(CALM_MAX_BATCH_BYTES),
                    "FASTDDS_CALM_MAX_SCHEDULED_BYTES": str(CALM_MAX_SCHEDULED_BYTES),
                    "FASTDDS_CALM_BUDGET_HORIZON_MS": str(CALM_BUDGET_HORIZON_MS),
                    "FASTDDS_CALM_DELTA_THRESHOLD_BYTES": str(CALM_DELTA_THRESHOLD_BYTES),
                    "FASTDDS_CALM_ONSET_THRESHOLD_BYTES": str(CALM_ONSET_THRESHOLD_BYTES),
                    "FASTDDS_CALM_MIN_REPAIR_RATE_MBPS": str(CALM_MIN_REPAIR_RATE_MBPS),
                    "FASTDDS_CALM_INITIAL_REPAIR_RATE_MBPS": str(
                        CALM_INITIAL_REPAIR_RATE_MBPS
                    ),
                    "FASTDDS_CALM_MAX_REPAIR_RATE_MBPS": str(CALM_MAX_REPAIR_RATE_MBPS),
                    "FASTDDS_CALM_HELD_NEW_RATE_MBPS": str(CALM_HELD_NEW_RATE_MBPS),
                    "FASTDDS_CALM_RETRY_COOLDOWN_MS": str(CALM_RETRY_COOLDOWN_MS),
                    "FASTDDS_CALM_FIRST_REPAIR_DELAY_MS": str(CALM_FIRST_REPAIR_DELAY_MS),
                    "FASTDDS_CALM_POST_REPAIR_GUARD_MS": str(CALM_POST_REPAIR_GUARD_MS),
                    "FASTDDS_CALM_RECOVERY_PROBE_DELAY_MS": str(
                        CALM_RECOVERY_PROBE_DELAY_MS
                    ),
                    "FASTDDS_CALM_MIN_PATH_RATE_MBPS": str(CALM_MIN_PATH_RATE_MBPS),
                    "FASTDDS_CALM_PATH_GAMMA": str(CALM_PATH_GAMMA),
                    "FASTDDS_CALM_PATH_ALPHA": str(CALM_PATH_ALPHA),
                    "FASTDDS_CALM_HOLD_MODE": str(CALM_HOLD_MODE),
                    "FASTDDS_CALM_KP": str(CALM_KP),
                    "FASTDDS_CALM_KI": str(CALM_KI),
                    "FASTDDS_CALM_KD": str(CALM_KD),
                    "FASTDDS_CALM_STORM_INITIAL_BUDGET_BYTES": str(
                        CALM_STORM_INITIAL_BUDGET_BYTES
                    ),
                    "FASTDDS_CALM_STORM_MIN_BUDGET_BYTES": str(
                        CALM_STORM_MIN_BUDGET_BYTES
                    ),
                    "FASTDDS_CALM_STORM_AI_BYTES": str(CALM_STORM_AI_BYTES),
                    "FASTDDS_CALM_MIN_BUDGET_SAMPLE_MULTIPLIER": str(
                        CALM_MIN_BUDGET_SAMPLE_MULTIPLIER
                    ),
                    "FASTDDS_CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER": str(
                        CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER
                    ),
                    "FASTDDS_CALM_MAX_BUDGET_SAMPLE_MULTIPLIER": str(
                        CALM_MAX_BUDGET_SAMPLE_MULTIPLIER
                    ),
                    "FASTDDS_CALM_AI_B_SAMPLE_MULTIPLIER": str(
                        CALM_AI_B_SAMPLE_MULTIPLIER
                    ),
                    "FASTDDS_CALM_BUDGET_SCALING_MODE": CALM_BUDGET_SCALING_MODE,
                    "FASTDDS_CALM_STORM_GAMMA": str(CALM_STORM_GAMMA),
                    "FASTDDS_CALM_STORM_RATE_GAMMA": str(CALM_STORM_RATE_GAMMA),
                    "FASTDDS_CALM_FIRST_FAILURE_GAMMA": str(
                        CALM_FIRST_FAILURE_GAMMA
                    ),
                    "FASTDDS_CALM_FIRST_FAILURE_RATE_GAMMA": str(
                        CALM_FIRST_FAILURE_RATE_GAMMA
                    ),
                    "FASTDDS_CALM_STORM_RATE_AI_MBPS": str(CALM_STORM_RATE_AI_MBPS),
                    "FASTDDS_CALM_MIN_RATE_OFFERED_MULTIPLIER": str(
                        CALM_MIN_RATE_OFFERED_MULTIPLIER
                    ),
                    "FASTDDS_CALM_INITIAL_RATE_OFFERED_MULTIPLIER": str(
                        CALM_INITIAL_RATE_OFFERED_MULTIPLIER
                    ),
                    "FASTDDS_CALM_MAX_RATE_OFFERED_MULTIPLIER": str(
                        CALM_MAX_RATE_OFFERED_MULTIPLIER
                    ),
                    "FASTDDS_CALM_AI_V_OFFERED_MULTIPLIER": str(
                        CALM_AI_V_OFFERED_MULTIPLIER
                    ),
                    "FASTDDS_CALM_OFFERED_RATE_EWMA_ALPHA": str(
                        CALM_OFFERED_RATE_EWMA_ALPHA
                    ),
                    "FASTDDS_CALM_PACING_DRAIN_GUARD": str(CALM_PACING_DRAIN_GUARD),
                    "FASTDDS_CALM_DETECTOR_RHO_BYTES": str(CALM_DETECTOR_RHO_BYTES),
                    "FASTDDS_CALM_DETECTOR_RHO_SAMPLE_MULTIPLIER": str(
                        CALM_DETECTOR_RHO_SAMPLE_MULTIPLIER
                    ),
                    "FASTDDS_CALM_DETECTOR_RETRY_COUNT": str(CALM_DETECTOR_RETRY_COUNT),
                    "FASTDDS_CALM_DETECTOR_RETRY_SIGNAL": CALM_DETECTOR_RETRY_SIGNAL,
                    "FASTDDS_CALM_DETECTOR_FAILED_REPAIR_COUNT": str(
                        CALM_DETECTOR_FAILED_REPAIR_COUNT
                    ),
                    "FASTDDS_CALM_DETECTOR_GROWTH_ROUNDS": str(
                        CALM_DETECTOR_GROWTH_ROUNDS
                    ),
                    "FASTDDS_CALM_DETECTOR_ACK_STALL_MS": str(
                        CALM_DETECTOR_ACK_STALL_MS
                    ),
                    "FASTDDS_CALM_ACK_STALL_MODE": CALM_ACK_STALL_MODE,
                    "FASTDDS_CALM_STORM_FEEDBACK_MIN_AGE_MS": str(
                        CALM_STORM_FEEDBACK_MIN_AGE_MS
                    ),
                    "FASTDDS_CALM_FEEDBACK_GUARD_MODE": CALM_FEEDBACK_GUARD_MODE,
                })
            pub_env["CALM_USE_RECEIVE_PROGRESS"] = "1"
            pub_env["CALM_CASE_D_TRIGGERS"] = ",".join(map(str, CASE_D_TRIGGER_COUNTS))
            pub_env["CALM_CASE_D_TRIGGERS"] = ",".join(map(str, CASE_D_TRIGGER_COUNTS))
            pub_env["CALM_CASE_D_DURATIONS_S"] = ",".join(
                map(str, CASE_D_DURATION_SECS_LIST)
            )
            pub_env["CALM_CASE_A_LOSS_PCT"] = str(
                case_a_loss_pct if case_a_enabled else 0
            )
            pub_env["FASTDDS_STORM_OBSERVER_ENABLED"] = (
                "1" if STORM_OBSERVER_ACTIVE and IS_FASTDDS else "0"
            )
            pub_env["FASTDDS_STORM_LOG_FILE"] = storm_log_path
            pub_env["CYCLONEDDS_STORM_OBSERVER_ENABLED"] = (
                "1" if STORM_OBSERVER_ACTIVE and IS_CYCLONEDDS else "0"
            )
            pub_env["CYCLONEDDS_STORM_LOG_FILE"] = storm_log_path
            pub_env["ROS_DOMAIN_ID"] = "25"
            pub_env["RMW_IMPLEMENTATION"] = RMW_IMPLEMENTATION
            pub_env["ROS_LOCALHOST_ONLY"] = "0"
            pub_env.update(get_rmw_xml_env_var(ACTIVE_LOCAL_DDS_XML))
            pub_env["CALM_NET_INTERFACE"] = CALM_NET_INTERFACE
            monitor_stop, monitor_thread = start_network_monitor(network_log_path)
            p_pub = subprocess.Popen(pub_cmd, env=pub_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.PIPE, text=True, bufsize=1, universal_newlines=True)
            
            # Publisher 스레드 시작
            t_pub = threading.Thread(target=stream_output, args=('PUB', p_pub.stdout, pub_output))
            t_pub.start()

            case_d_stop = None
            case_d_thread = None
            case_d_state = None
            if case_d_enabled:
                case_d_stop, case_d_thread, case_d_state = start_case_d_supervisor(
                    pub_output, p_pub, p_sub
                )

            case_l_summary = "off"
            if case_l_enabled:
                case_l_summary = start_case_l_background(
                    idx, loss, max_samples, pub_socket_buffer, sub_socket_buffer,
                    pub_output, p_pub, p_sub, bg_count, event_log_path,
                    case_l_enabled=case_l_enabled
                )

            # 3) Subscriber가 종료될 때까지 대기
            exit_mode = "normal"  # normal / sub_force / pub_force / sub_force,pub_force
            try:
                # SUB_STATS가 나오면 최대 10초 더 대기 후 강제 종료
                sub_stats_timeout = 30.0
                sub_stats_found = False
                start_sub_wait = time.time()
                
                # SUB_STATS 확인 루프 (40초 안에 미종료 시 강제 종료 후에도 측정 결과 출력/기록)
                while (time.time() - start_sub_wait) < 300:
                    if any('[SUB_STATS]' in line for line in sub_output):
                        if not sub_stats_found:
                            sub_stats_found = True
                            debug_log("SUB_STATS found, waiting for subscriber to finish...")
                            start_sub_wait = time.time()  # SUB_STATS 발견 시점부터 재시작
                        elif (time.time() - start_sub_wait) >= sub_stats_timeout:
                            debug_log(f"SUB_STATS found but subscriber not terminated after {sub_stats_timeout}s, forcing termination")
                            break
                    
                    # 프로세스가 이미 종료되었는지 확인
                    if p_sub.poll() is not None:
                        debug_log("Subscriber process terminated")
                        break
                    
                    time.sleep(0.5)
                
                # 프로세스가 아직 실행 중이면 강제 종료 (끄기 전에 지금까지 출력 수집)
                if p_sub.poll() is None:
                    exit_mode = "sub_force"
                    debug_log("Forcing subscriber termination (will still output/record measurement and exit)")
                    try:
                        time.sleep(0.5)  # 파이프에 남은 출력을 reader가 읽을 시간
                        p_sub.terminate()
                        time.sleep(1.0)
                        t_sub.join(timeout=2)  # 마지막 출력을 sub_output에 모은 뒤 진행
                        if p_sub.poll() is None:
                            p_sub.kill()
                    except Exception:
                        pass
                    kill_remote_sub(force=True)
                else:
                    # 정상 종료 대기
                    p_sub.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    p_sub.kill()
                except Exception:
                    pass
                kill_remote_sub(force=True)

            if case_d_enabled and case_d_stop is not None:
                stop_case_d_supervisor(
                    case_d_stop, case_d_thread, case_d_state
                )

            
            # Publisher 통계([PUB_STATS])가 나올 때까지 잠시 대기 후 종료
            pub_stats_timeout = False
            try:
                wait_step = 0.2
                start_wait = time.time()
                theoretical_duration = max(0.0, float(MAX_COUNT) * float(publish_period))+40
                while True:
                    if any('[PUB_STATS]' in line for line in pub_output):
                        elapsed = time.time() - start_wait
                        debug_log(f"Observed PUB_STATS after waiting {elapsed:.1f}s")
                        break
                    if (time.time() - start_wait) >= theoretical_duration:
                        pub_stats_timeout = True
                        debug_log(f"Reached theoretical pub duration {theoretical_duration:.3f}s without PUB_STATS; proceeding to terminate")
                        break
                    time.sleep(wait_step)
                # 추가 5초 대기 후 정상 종료 시도
                debug_log("Waiting 5s before terminating publisher...")
                time.sleep(5.0)
                
                # 정상 종료 시도 (끄기 전에 지금까지 출력 수집)
                debug_log("Terminating publisher...")
                p_pub.terminate()
                time.sleep(1.0)
                t_pub.join(timeout=2)  # 마지막 출력을 pub_output에 모은 뒤 진행
                if p_pub.poll() is None:
                    debug_log("Publisher still running, killing...")
                    p_pub.kill()
                else:
                    debug_log("Publisher terminated successfully")
            except Exception as e:
                debug_log(f"Exception during publisher termination: {e}")
            
            # 백업: 프로세스가 아직 실행 중인 경우에만 강제 종료 (출력 수집 후)
            if p_pub.poll() is None:  # 아직 실행 중이면
                if exit_mode == "normal":
                    exit_mode = "pub_force"
                else:
                    exit_mode = "sub_force,pub_force"
                debug_log("Publisher still running, forcing kill...")
                try:
                    time.sleep(0.5)
                    t_pub.join(timeout=2)
                except Exception:
                    pass
                kill_remote_sub(force=True)
                kill_local_pub(force=True)  # 로컬 퍼블리셔도 정리
            elif pub_stats_timeout:
                if exit_mode == "normal":
                    exit_mode = "pub_force"
                else:
                    exit_mode = "sub_force,pub_force"
                debug_log("Publisher process confirmed terminated")
            else:
                debug_log("Publisher process confirmed terminated")

            # 스레드 종료 대기 (파이프를 닫기 전에 먼저 스레드 종료)
            debug_log("Waiting for threads to join...")
            try:
                t_pub.join(timeout=3)
                debug_log("Publisher thread joined")
            except Exception as e:
                debug_log(f"Error joining pub thread: {e}")
            try:
                t_sub.join(timeout=3)
                debug_log("Subscriber thread joined")
            except Exception as e:
                debug_log(f"Error joining sub thread: {e}")

            stop_network_monitor(monitor_stop, monitor_thread)
            if case_d_enabled:
                write_case_d_event_log(event_log_path, pub_output)
                restore_local_tc_baseline()
            elif case_a_enabled:
                baseline_restored = False
                restore_error = None
                try:
                    restore_local_tc_baseline()
                    baseline_restored = True
                except Exception as exc:
                    restore_error = exc
                write_case_a_event_log(
                    event_log_path, pub_output, case_a_loss_pct, baseline_restored
                )
                if restore_error is not None:
                    raise restore_error
            else:
                restore_local_tc_baseline()

            # 파이프를 명시적으로 닫기 (스레드 종료 후)
            # 주의: close()가 블로킹될 수 있으므로 선택적으로 처리
            debug_log("Attempting to close pipes (non-blocking)...")
            try:
                if p_pub.stdout and not p_pub.stdout.closed:
                    debug_log("Closing publisher stdout...")
                    # 파이프를 닫는 것을 시도하되, 블로킹을 방지하기 위해 별도 스레드에서 실행
                    def close_pub_stdout():
                        try:
                            p_pub.stdout.close()
                            debug_log("Publisher stdout closed")
                        except Exception as e:
                            debug_log(f"Error closing pub stdout: {e}")
                    t_close_pub = threading.Thread(target=close_pub_stdout, daemon=True)
                    t_close_pub.start()
                    t_close_pub.join(timeout=1.0)  # 최대 1초 대기
                    if t_close_pub.is_alive():
                        debug_log("Publisher stdout close timed out, continuing...")
                else:
                    debug_log("Publisher stdout already closed or None")
            except Exception as e:
                debug_log(f"Error in pub stdout close attempt: {e}")
            
            try:
                if p_sub.stdout and not p_sub.stdout.closed:
                    debug_log("Closing subscriber stdout...")
                    # 파이프를 닫는 것을 시도하되, 블로킹을 방지하기 위해 별도 스레드에서 실행
                    def close_sub_stdout():
                        try:
                            p_sub.stdout.close()
                            debug_log("Subscriber stdout closed")
                        except Exception as e:
                            debug_log(f"Error closing sub stdout: {e}")
                    t_close_sub = threading.Thread(target=close_sub_stdout, daemon=True)
                    t_close_sub.start()
                    t_close_sub.join(timeout=1.0)  # 최대 1초 대기
                    if t_close_sub.is_alive():
                        debug_log("Subscriber stdout close timed out, continuing...")
                else:
                    debug_log("Subscriber stdout already closed or None")
            except Exception as e:
                debug_log(f"Error in sub stdout close attempt: {e}")
            debug_log("Pipe close attempt completed (pipes will be closed automatically when process exits)")

            # 프로세스가 완전히 종료될 때까지 대기
            debug_log("Waiting for processes to fully terminate...")
            time.sleep(2.0)
            
            # 남은 DDS 프로세스 강제 종료
            debug_log("Killing any remaining DDS processes...")
            kill_local_pub(force=True)
            kill_remote_sub(force=True)

            # Subscriber 노트북에서 생성된 개별 CSV를 로컬 결과 폴더로 수집합니다.
            fetch_remote_sub_csv(
                loss, idx, payload, hz, max_samples, sub_socket_buffer, burst_secs, payload_type,
                topic_prefix=MAIN_TOPIC_PREFIX, qos_mode=MAIN_QOS_MODE, max_count=MAX_COUNT,
                result_suffix=result_suffix
            )
            
            # 로컬에서 ros2 daemon 재시작 (공유 메모리 정리)
            debug_log("Restarting ros2 daemon for cleanup...")
            try:
                subprocess.run(["ros2", "daemon", "stop"], 
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
                time.sleep(1.0)
                # FastDDS 공유 메모리 정리
                subprocess.run(["rm", "-rf", "/dev/shm/fastdds*"], 
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                subprocess.run(["rm", "-rf", "/dev/shm/cyclonedds*"], 
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(1.0)
                subprocess.run(["ros2", "daemon", "start"], 
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
                debug_log("ros2 daemon restarted")
            except Exception as e:
                debug_log(f"Error restarting ros2 daemon: {e}")
            
            time.sleep(1.0)
            
            # reset 스크립트는 sysctl/DDS 튜닝을 포함할 수 있어 실행하지 않습니다.
            
            # 로컬 reset 후 대기 (ros2 daemon 재시작 시간)
            time.sleep(2.0)

            # 원격도 동일하게 기본 시스템 설정을 유지합니다.
            
            # 원격 reset 후 대기
            time.sleep(2.0)

            # UDP 통계 수집 (테스트 후)
            debug_log("Collecting UDP stats after test...")
            udp_stats_after = get_udp_stats()
            debug_log(f"UDP stats collected: {udp_stats_after}")

            # 통합 로그 출력 및 CSV 저장
            debug_log("Calling print_unified_log...")
            print_unified_log(
                idx, payload, loss, payload_type, burst_secs, max_samples, pub_socket_buffer,
                sub_socket_buffer, publish_period, MAX_COUNT, csv_writer, csvfile, sub_output,
                pub_output, udp_stats_before, udp_stats_after, exit_mode, case_l_summary,
                scenario_name, case_d_enabled, case_l_enabled, case_a_enabled,
                case_a_loss_pct, calm_beta, calm_gamma, calm_alpha, calm_q_min,
                bg_count, optimizer_parameters
            )
            debug_log("print_unified_log completed")

            # 테스트 완료 메시지
            debug_log("Printing test completion message...")
            print(f"✅ 테스트 완료 - idx={idx} | 페이로드={payload_kb}KB | 헤르츠={hz:.1f}Hz")
            print()
            debug_log(f"Test idx={idx} completed, waiting 3s before next test for cleanup...")
            
            # print(f"[SYS] Test idx={idx} complete.\n")
            # 각 테스트 사이에 충분한 대기 시간 (DDS 리소스 정리 및 네트워크 안정화)
            time.sleep(3.0)
            # Zenoh: 이 세션 라우터 종료
            if RMW_IMPLEMENTATION == "rmw_zenoh_cpp" and zenoh_routers_started:
                debug_log("Stopping Zenoh routers (end of session)...")
                kill_zenoh_router_local(p_zenoh_router_local)
                kill_zenoh_router_remote(p_zenoh_router_remote)
            debug_log("Ready for next test iteration")
    
    finally:
        try:
            kill_remote_sub(force=True)
        except Exception as exc:
            debug_log(f"Final remote Subscriber cleanup failed: {exc}")
        try:
            kill_local_pub(force=True)
        except Exception as exc:
            debug_log(f"Final local Publisher cleanup failed: {exc}")
        try:
            restore_local_tc_baseline()
        except Exception as exc:
            debug_log(f"Final tc baseline restore failed: {exc}")

        # Zenoh 라우터 종료
        if RMW_IMPLEMENTATION == "rmw_zenoh_cpp" and zenoh_routers_started:
            debug_log("Stopping Zenoh routers...")
            kill_zenoh_router_local(p_zenoh_router_local)
            kill_zenoh_router_remote(p_zenoh_router_remote)
            debug_log("Zenoh routers stopped")
        
        # CSV 파일 닫기
        csvfile.close()
        print(f"[SYS] All tests completed. Results saved to {result_csv_path}")



if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        kill_remote_sub(force=True)
        kill_local_pub(force=True)
        # Zenoh 라우터 종료
        if RMW_IMPLEMENTATION == "rmw_zenoh_cpp":
            kill_zenoh_router_local()
            kill_zenoh_router_remote()
        sys.exit(1)
