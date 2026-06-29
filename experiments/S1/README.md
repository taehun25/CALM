# S1: CALM Pretest Baseline Experiment

S1 is a snapshot of the current `calm_pretest` ROS 2 package used for the first
pre-CALM baseline experiment.

## Purpose

This experiment compares `RELIABLE` and `BEST_EFFORT` DDS communication before
applying the CALM algorithm. The main goal is to observe whether reliable DDS
communication shows performance degradation under wireless conditions, especially
when large payloads and retransmission behavior are involved.

## Experiment Setup

- Publisher laptop: runs `calm_automation` and `calm_pub`
- Subscriber laptop: runs `calm_sub` through SSH
- ROS 2 workspace path on publisher: `/home/csi/ros2_ws`
- ROS 2 workspace path on subscriber: `/home/csilab/ros2_ws`
- Default ROS domain ID: `117`
- Default RMW implementation: `rmw_fastrtps_cpp`
- Default history QoS: `KEEP_ALL`

## Sweep Parameters

Default S1 matrix:

- reliability: `reliable`, `best_effort`
- payload: `32KB`, `64KB`, `128KB`, `256KB`, `512KB`
- publish Hz: `5`, `10`, `20`, `30`, `50`
- repeat: `5`

The package also supports short debugging runs with overrides:

```bash
ros2 run calm_pretest calm_automation \
  --remote-host <SUB_IP> \
  --payload-bytes 1024 \
  --hz 5 \
  --reliability reliable \
  --count 100 \
  --repeat 1 \
  --max-runs 1
```

## Package Contents

The copied ROS 2 package is stored at:

```text
experiments/S1/calm_pretest/
```

Important files:

- `src/calm_pub.cpp`: publisher node
- `src/calm_sub.cpp`: subscriber node
- `src/calm_automation.cpp`: SSH-based automation runner
- `CMakeLists.txt`: build configuration
- `package.xml`: ROS 2 package metadata
- `README.md`: package-level usage notes

## Current Implementation Notes

- Each run uses unique topic names derived from `run_id`.
- Publisher waits for both subscriber-ready signaling and data-topic matching.
- Automation starts the remote subscriber through SSH and stores CSV/log files
  under `results/pretest_csv/`.
- Packet loss injection is not implemented in S1. The code only records observed
  receive ratio, delay, duplicate count, and measured receive Hz.

## Debugging Notes

On an internet-connected Wi-Fi network, automation succeeded for small payloads
but large payload delivery failed:

- `1KB`, reliable, `5Hz`, count `100`: `100/100` received
- `2KB`, reliable, `5Hz`, count `30`: `1/30` received
- `4KB`, reliable, `5Hz`, count `30`: `0/30` received
- `8KB`, reliable, `5Hz`, count `30`: `0/30` received
- `32KB`, reliable, `5Hz`, count `100`: `0/100` received

This suggests that the internet Wi-Fi network is unsuitable for the planned
large-payload DDS experiment. The dedicated experiment router should be used for
the actual S1 baseline run.
