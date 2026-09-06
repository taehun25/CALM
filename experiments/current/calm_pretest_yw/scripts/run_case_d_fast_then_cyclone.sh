#!/usr/bin/env bash

set -eo pipefail

WS=/home/csi/ros2_ws
AUTOMATION="$WS/src/calm_pretest_yw/scripts/automation.py"
RESULT_ROOT="$WS/results/test_yw_2"
REMOTE_RESULT_ROOT=/home/csilab/ros2_ws/results/test_yw_2
INTERFACE=wlp2s0
LOCAL_IP=192.168.0.2
REMOTE_IP=192.168.0.3

cd "$WS"

# ROS setup scripts may read optional variables before defining them.
set +u
source /opt/ros/humble/setup.bash
source "$WS/install/setup.bash"
set -u

if ! ip -o -4 addr show dev "$INTERFACE" | grep -Fq "inet $LOCAL_IP/"; then
    echo "[ERROR] $INTERFACE에 실험용 주소 $LOCAL_IP가 없습니다."
    echo "먼저 실험용 Wi-Fi에 연결한 뒤 다시 실행하세요."
    exit 1
fi

if ! ping -c 2 -W 2 "$REMOTE_IP" >/dev/null; then
    echo "[ERROR] Subscriber $REMOTE_IP에 연결할 수 없습니다."
    exit 1
fi

export CALM_NETWORK_MODE=wifi
export CALM_NET_INTERFACE="$INTERFACE"
export CALM_REMOTE_HOST="$REMOTE_IP"

export CALM_STORM_ENABLED=0
export CALM_RUN_SELECTED_ONLY=1
export CALM_SELECTED_IDXS="$(seq -s, 1 36)"

export CALM_CASE_D_TRIGGER_COUNTS=200,1000
export CALM_CASE_D_DURATION_SECS=20,40
export CALM_SUB_INACTIVE_TIMEOUT_S=300

# DDS Optimizer rules 1 and 2 only.
export DDS_WIRELESS_OPTIMIZER=1
export DDS_OPT_HISTORY_CACHE_ENABLED=0
export DDS_OPT_MAX_MESSAGE_SIZE=1472

TAG="$(date +%Y%m%d_%H%M%S)"

run_case_d()
{
    local vendor="$1"
    local label="$2"
    local run_name="${label}_caseD_wifi_${TAG}"

    export CALM_DDS_VENDOR="$vendor"
    unset RMW_IMPLEMENTATION || true

    export CALM_EXPERIMENT_RESULT_DIR="$RESULT_ROOT/$run_name"
    export CALM_REMOTE_RESULT_DIR="$REMOTE_RESULT_ROOT/$run_name"

    mkdir -p "$CALM_EXPERIMENT_RESULT_DIR"

    echo "============================================================"
    echo "CASE D 시작: $label"
    echo "Publisher/Subscriber DDS: $vendor"
    echo "결과 폴더: $CALM_EXPERIMENT_RESULT_DIR"
    echo "============================================================"

    python3 "$AUTOMATION" 2>&1 |
        tee "$CALM_EXPERIMENT_RESULT_DIR/automation_console.log"

    echo "CASE D 완료: $label"
}

# A test always uses the same RMW on its publisher and subscriber.
run_case_d fastdds FastDDSOPT12_default
run_case_d cyclonedds CycloneDDSOPT12_default

echo "============================================================"
echo "Fast DDS 및 Cyclone DDS CASE D 실험 완료"
echo "실험 TAG: $TAG"
echo "============================================================"
