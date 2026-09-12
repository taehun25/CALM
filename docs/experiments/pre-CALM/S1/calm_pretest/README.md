# CALM Pretest: RELIABLE vs BEST_EFFORT Capacity Baseline

This package runs the first pre-CALM baseline experiment:

- same payload size
- same publish rate
- same `KEEP_ALL` history
- only reliability changes: `RELIABLE` vs `BEST_EFFORT`

The publisher laptop runs `calm_automation`. The subscriber laptop runs `calm_sub` through SSH. The publisher starts only after it receives `calm_pretest_sub_ready`.
If the ready topic is not discovered, the publisher also accepts a matched `calm_pretest_data` subscriber as a valid handshake.

## Files

- `src/calm_pub.cpp`: local publisher node
- `src/calm_sub.cpp`: remote subscriber node
- `src/calm_automation.cpp`: experiment runner

## Build

On both laptops, place this package under the ROS 2 workspace `src` tree and build:

```bash
cd /home/csi/ros2_ws
source /opt/ros/humble/setup.bash
colcon build --packages-select calm_pretest
```

On the subscriber laptop, replace the workspace path if needed:

```bash
cd /home/csilab/ros2_ws
source /opt/ros/humble/setup.bash
colcon build --packages-select calm_pretest
```

## Run Automation From Publisher Laptop

```bash
cd /home/csi/ros2_ws
source /opt/ros/humble/setup.bash
source install/setup.bash
ros2 run calm_pretest calm_automation --remote-host <SUB_LAPTOP_IP>
```

Useful options:

```bash
--remote-user csilab
--remote-ws /home/csilab/ros2_ws
--local-ws /home/csi/ros2_ws
--domain-id 117
--count 1000
--repeat 5
--max-runs 0
--payload-bytes 32768
--hz 5
--reliability reliable
--ready-timeout-s 120
--idle-timeout-s 30
--total-timeout-s 240
--sub-start-delay-s 10
--inter-run-delay-s 5
--rmw rmw_fastrtps_cpp
--localhost-only 0
```

The default subscriber workspace is `/home/csilab/ros2_ws`, so the package should be placed at `/home/csilab/ros2_ws/src/calm_pretest`.

For a short automation smoke test before an overnight run:

```bash
ros2 run calm_pretest calm_automation --remote-host 192.168.50.53 --count 100 --repeat 1 --max-runs 1
```

For network debugging with a smaller payload:

```bash
ros2 run calm_pretest calm_automation --remote-host <SUB_IP> --payload-bytes 1024 --hz 5 --reliability reliable --count 100 --repeat 1 --max-runs 1
```

The automation runner cleans up stale remote `calm_sub` processes before each run, waits for the remote subscriber to settle, then starts the publisher. It explicitly sets `ROS_DOMAIN_ID`, `ROS_LOCALHOST_ONLY`, and `RMW_IMPLEMENTATION` for both laptops and writes those values at the top of each `pub_stdout.log` and `sub_stdout.log`.

## Experiment Matrix

The automation runner currently sweeps:

- reliability: `reliable`, `best_effort`
- payload: `32KB`, `64KB`, `128KB`, `256KB`, `512KB`
- publish Hz: `5`, `10`, `20`, `30`, `50`
- repeat: `5`

Total runs: `2 x 5 x 5 x 5 = 250`.

## Output

Results are collected on the publisher laptop under:

```text
results/pretest_csv/reliability_vs_besteffort_capacity_<YYYYMMDD_HHMMSS>/
```

Each run directory contains:

- `*_pub_samples.csv`
- `*_pub_summary.csv`
- `*_sub_samples.csv`
- `*_sub_summary.csv`
- `pub_stdout.log`
- `sub_stdout.log`

The batch directory also contains:

- `runs_manifest.csv`

The main metric for this first experiment is in `*_sub_summary.csv`:

- `measured_sub_hz`
- `receive_ratio`
- `avg_delay_ms`
- `p95_delay_ms`
- `p99_delay_ms`
