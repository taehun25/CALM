# CALM 4.3 Fast DDS experiments

`calm_pretest_yw/` is the ROS 2 Humble experiment package used for the CALM 4.3
Fast DDS loopback validation. Cyclone-specific profiles and implementation are
not included in this release.

## Build

```bash
cd ~/ros2_ws
source /opt/ros/humble/setup.bash
colcon build --symlink-install --packages-select calm_pretest_yw
source install/setup.bash
```

## Run the standard matrix

Place or symlink `calm_pretest_yw/` under the workspace `src` directory, then:

```bash
cd ~/ros2_ws
CALM_CONTROLLER=calm43 \
PAYLOADS_KB="256 512 1024" \
PUBLISH_RATES_HZ="20 30" \
SAMPLE_COUNT=2000 \
./src/calm_pretest_yw/scripts/run_fastdds_calm42_validation.sh normal_return_g
```

The script name retains `calm42` for backward-compatible experiment replay;
`CALM_CONTROLLER=calm43` selects the current controller. Each scenario runs an
Optimized Default control and CALM pair. Root privileges are required only for
the loopback `tc netem` configuration.

## Scenarios

- `baseline`: no injected packet loss
- `case_a`: transient PER 10% for 20 seconds after the trigger
- `per`: persistent PER 10%
- `case_d`: temporary 100% link loss
- `case_l`: temporary competing background traffic

Generated Fast DDS XML applies `maxMessageSize=1472 B` and a periodic
HEARTBEAT period equal to half the application publish period to both control
and CALM runs.
