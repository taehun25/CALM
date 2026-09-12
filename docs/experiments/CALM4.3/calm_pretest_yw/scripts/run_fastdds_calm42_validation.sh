#!/usr/bin/env bash
set -euo pipefail

# Explicit CALM 4.2 experiment launcher. It runs only when invoked by a user.
# Environment variables below may be overridden before invocation.
WS="${ROS2_WS:-/home/csi/ros2_ws}"
RUNNER="${WS}/src/calm_pretest_yw/scripts/ddsopt_loopback_automation.py"
RESULT_ROOT="${CALM_RESULT_ROOT:-${WS}/results/test_yw_2}"

# The Cartesian product covers low load through sustained overload on the
# 180 Mbps loopback link. Override either comma-separated list when needed.
PAYLOADS_KB="${PAYLOADS_KB:-256,512,1024}"
PUBLISH_RATES_HZ="${PUBLISH_RATES_HZ:-20,30}"
SAMPLE_COUNT="${SAMPLE_COUNT:-2000}"
CALM_CONTROLLER="${CALM_CONTROLLER:-calm42}"
PER_PERCENT="${PER_PERCENT:-10}"
CASE_A_PER_PERCENT="${CASE_A_PER_PERCENT:-10}"
CASE_A_SECONDS="${CASE_A_SECONDS:-20}"
CASE_D_SECONDS="${CASE_D_SECONDS:-20}"
CASE_L_SECONDS="${CASE_L_SECONDS:-20}"
BG_PAYLOAD_KB="${BG_PAYLOAD_KB:-512}"
BG_HZ="${BG_HZ:-20}"
BG_FLOWS="${BG_FLOWS:-1}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-300}"
DOMAIN_BASE="${DOMAIN_BASE:-190}"

if [[ ! -f "${RUNNER}" ]]; then
    printf 'Automation runner not found: %s\n' "${RUNNER}" >&2
    exit 1
fi

# ROS setup scripts may inspect an unset AMENT_TRACE_SETUP_FILES variable.
set +u
source /opt/ros/humble/setup.bash
source "${WS}/install/setup.bash"
set -u

sudo -v
cleanup() {
    sudo tc qdisc del dev lo root >/dev/null 2>&1 || true
}
trap cleanup EXIT
trap 'exit 130' INT TERM

COMMON=(
    --dds fastdds
    --result-root "${RESULT_ROOT}"
    --modes opt12
    --controls default,calm
    --payloads-kb "${PAYLOADS_KB}"
    --hertz "${PUBLISH_RATES_HZ}"
    --count "${SAMPLE_COUNT}"
    --max-samples "${SAMPLE_COUNT}"
    --timeout-s "${TIMEOUT_SECONDS}"
    --domain-base "${DOMAIN_BASE}"
    --link-rate-mbit 180
    --delay-ms 3
    --jitter-ms 1
    --qdisc-limit 0
    --calm-controller "${CALM_CONTROLLER}"
    --calm-entry-f-threshold 1
    --calm-entry-f-mode delta_f
    --calm4-k-dec 0.25
    --calm4-k-inc 0.25
    --calm-min-budget-bytes 131072
    --calm-initial-budget-bytes 524288
    --calm-max-budget-bytes 2097152
    --calm-min-budget-sample-multiplier 0.25
    --calm-initial-budget-sample-multiplier 1.0
    --calm-max-budget-sample-multiplier 2.0
    --calm-ai-b-sample-multiplier 0.25
    --calm-feedback-min-age-ms 100
    --calm41-timeout-rtt-multiplier 4
)

run_case() {
    local label="${1/calm42/${CALM_CONTROLLER}}"
    shift
    python3 "${RUNNER}" --label "${label}" "${COMMON[@]}" "$@"
}

run_entry_f_case() {
    local threshold="$1"
    local label="${2/calm42/${CALM_CONTROLLER}}"
    shift 2
    python3 "${RUNNER}" --label "${label}_entryF${threshold}" \
        "${COMMON[@]}" --calm-entry-f-mode absolute_f \
        --calm-entry-f-threshold "${threshold}" "$@"
}

run_entry_f_scenario() {
    local scenario="$1"
    local threshold
    for threshold in 1 2 3; do
        case "${scenario}" in
            baseline)
                run_entry_f_case "${threshold}" calm42_baseline \
                    --scenarios normal --baseline-loss-percent 0
                ;;
            case_a)
                run_entry_f_case "${threshold}" calm42_caseA${CASE_A_PER_PERCENT} \
                    --scenarios case_a --baseline-loss-percent 0 \
                    --case-a-loss-percent "${CASE_A_PER_PERCENT}" \
                    --case-a-duration-s "${CASE_A_SECONDS}"
                ;;
            per)
                run_entry_f_case "${threshold}" calm42_persistent_per${PER_PERCENT} \
                    --scenarios normal --baseline-loss-percent "${PER_PERCENT}"
                ;;
        esac
    done
}

selection="${1:-all}"
printf 'CALM controller=%s matrix: payloads=%s KiB, rates=%s Hz, selection=%s\n' \
    "${CALM_CONTROLLER}" "${PAYLOADS_KB}" "${PUBLISH_RATES_HZ}" "${selection}"
printf 'Each selected scenario runs every payload/rate pair for Default and CALM.\n'
case "${selection}" in
    per)
        run_case calm42_persistent_per${PER_PERCENT} --scenarios normal \
            --baseline-loss-percent "${PER_PERCENT}"
        ;;
    baseline)
        run_case calm42_baseline --scenarios normal --baseline-loss-percent 0
        ;;
    case_a)
        run_case calm42_caseA${CASE_A_PER_PERCENT} --scenarios case_a \
            --baseline-loss-percent 0 --case-a-loss-percent "${CASE_A_PER_PERCENT}" \
            --case-a-duration-s "${CASE_A_SECONDS}"
        ;;
    case_d)
        run_case calm42_caseD --scenarios case_d --baseline-loss-percent 0 \
            --case-duration-s "${CASE_D_SECONDS}"
        ;;
    case_l)
        run_case calm42_caseL --scenarios case_l --baseline-loss-percent 0 \
            --case-duration-s "${CASE_L_SECONDS}" \
            --bg-payload-kb "${BG_PAYLOAD_KB}" --bg-hz "${BG_HZ}" \
            --bg-flows "${BG_FLOWS}"
        ;;
    all)
        "${BASH_SOURCE[0]}" per
        "${BASH_SOURCE[0]}" baseline
        "${BASH_SOURCE[0]}" case_a
        "${BASH_SOURCE[0]}" case_d
        "${BASH_SOURCE[0]}" case_l
        ;;
    entry_f_sweep)
        # Keep scenario order fixed; only the CALM entry F threshold changes.
        run_entry_f_scenario baseline
        run_entry_f_scenario case_a
        run_entry_f_scenario per
        ;;
    pressure_gate)
        # CALM_ACTIVE = D_n = delta_F_old>=1 AND (delta_U>0 OR ACK stall).
        # Keep the requested experiment order fixed.
        run_case calm42_DnDeltaF_baseline --scenarios normal --baseline-loss-percent 0
        run_case calm42_DnDeltaF_caseA${CASE_A_PER_PERCENT} --scenarios case_a \
            --baseline-loss-percent 0 --case-a-loss-percent "${CASE_A_PER_PERCENT}" \
            --case-a-duration-s "${CASE_A_SECONDS}"
        run_case calm42_DnDeltaF_persistent_per${PER_PERCENT} --scenarios normal \
            --baseline-loss-percent "${PER_PERCENT}"
        ;;
    normal_return_g)
        # CALM_ACTIVE = D_n. Return directly to NORMAL when
        # U_n <= S_bar AND U_n < U_(n-1) AND delta_F_old,n = 0.
        # Keep the comparison order fixed and label it separately from the
        # earlier U=0-only pressure_gate runs.
        run_case calm42_DnDeltaF_Greturn_baseline \
            --scenarios normal --baseline-loss-percent 0
        run_case calm42_DnDeltaF_Greturn_caseA${CASE_A_PER_PERCENT} \
            --scenarios case_a --baseline-loss-percent 0 \
            --case-a-loss-percent "${CASE_A_PER_PERCENT}" \
            --case-a-duration-s "${CASE_A_SECONDS}"
        run_case calm42_DnDeltaF_Greturn_persistent_per${PER_PERCENT} \
            --scenarios normal --baseline-loss-percent "${PER_PERCENT}"
        ;;
    *)
        printf 'Usage: %s {all|normal_return_g|pressure_gate|entry_f_sweep|per|baseline|case_a|case_d|case_l}\n' "$0" >&2
        exit 2
        ;;
esac
