# CALM 4.3 Fast DDS implementation

This directory contains the current CALM 4.3 source overlay and patch for
eProsima Fast DDS `v2.6.11` (`87dd60c8f3e8694481ad0279bd4cc8c645050da3`).
It does not contain Cyclone DDS code.

## Install

```bash
git clone --branch v2.6.11 https://github.com/eProsima/Fast-DDS.git Fast-DDS
cd Fast-DDS
git apply /path/to/CALM/CALM/fastdds-v2.6.11/CALM.patch
```

The `overlay/` directory mirrors Fast DDS paths. It can be copied over an exact
Fast DDS 2.6.11 checkout for inspection or recovery, but applying the patch is
preferred because Git verifies the surrounding source context.

```bash
cp -a /path/to/CALM/CALM/fastdds-v2.6.11/overlay/. /path/to/Fast-DDS/
```

## Main implementation files

| File | Responsibility |
| --- | --- |
| `ChangeForReader.h` | Per-sample repair scope, retransmission rounds, re-NACK overlap, held-new state |
| `ReaderProxy.h/.cpp` | $U$, $\bar S$, CALM 4.3 entry/return, AIMD budget, feedback timing, limited oldest-first release |
| `StatefulWriter.h/.cpp` | Pacing event, repair-first shared budget, held-new release |
| `RTPSWriter.cpp` | Writer-side CALM observation hooks |
| `ReaderProxyTests.cpp` | Unit coverage for repair accounting and CALM state transitions |

## Runtime selection

The implementation is compiled into Fast DDS but remains disabled unless the
experiment enables it.

```bash
export FASTDDS_CALM_ENABLED=1
export FASTDDS_CALM_CONTROLLER=calm43
export FASTDDS_CALM_HOLD_MODE=1
export FASTDDS_CALM4_K_DEC=0.25
export FASTDDS_CALM4_K_INC=0.25
```

CALM 4.3 uses the Writer's configured periodic HEARTBEAT interval as $T_p$.
`FASTDDS_CALM_PACING_MS` is only the fallback when no valid heartbeat interval
is available. OPT 1 and OPT 2 are generated as Fast DDS XML profiles by the
experiment package; they are not hard-coded into this middleware patch.

## Build in ROS 2 Humble

```bash
cd ~/ros2_ws
source /opt/ros/humble/setup.bash
colcon build --base-paths src/Fast-DDS \
  --symlink-install \
  --packages-select fastrtps \
  --allow-overriding fastrtps
```

Then rebuild or source the workspace containing `rmw_fastrtps_cpp` as usual.
Upstream Fast DDS licensing applies to all derived source files.
