# CALM experiments

This directory is the current ROS 2 Humble experiment package used to compare
Optimized Default DDS and CALM.

- `src/`: publisher and subscriber nodes
- `config/`: Fast DDS and Cyclone DDS transport profiles
- `scripts/automation.py`: Wi-Fi/SSH experiment automation
- `scripts/ddsopt_loopback_automation.py`: reproducible loopback/netem A/B runs
- `scripts/calm_*`: parameter sweeps and validation utilities

The package name remains `calm_pretest_yw`. Place this directory at
`<ros2-workspace>/src/calm_pretest_yw`, resolve dependencies, and build it with
`colcon build --symlink-install`.

Raw result directories are intentionally excluded from this repository. The
curated numerical results and interpretations are in `../docs/`.
