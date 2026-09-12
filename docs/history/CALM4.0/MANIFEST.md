# CALM 4.0 backup

This directory preserves the CALM implementation immediately before the
CALM 4.1 controller was added on 2026-08-22.

## Baseline

- Fast DDS commit: `87dd60c8f3e8694481ad0279bd4cc8c645050da3`

## Contents

- `FastDDS_CALM4.0.patch`: binary-safe diff against the Fast DDS baseline.
- `README.md`: archive scope and current-release pointer.

The Fast DDS CALM 4.0 controller is selected with
`FASTDDS_CALM_CONTROLLER=calm4` and uses a fixed 50 ms pacing period.
