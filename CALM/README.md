# CALM middleware implementation

This directory contains the middleware patches and source overlays used to
implement CALM. It is the core implementation area of this repository, but it
is not a standalone library: apply the matching patch to the exact upstream
DDS version and rebuild that middleware and its ROS 2 RMW package.

- `fastdds-v2.6.11`: based on eProsima Fast DDS tag `v2.6.11`, commit
  `87dd60c8f3e8694481ad0279bd4cc8c645050da3`
- `cyclonedds-0.10.5`: based on Eclipse Cyclone DDS tag `0.10.5`, commit
  `2cdd114cbd18340c606573b4cc8dc20cc161ec5a`

The Fast DDS patch contains the current CALM 4.1 implementation. The Cyclone
DDS patch is the earlier CALM 3-series port and has not yet been unified with
the complete CALM 4.1 controller.

The patch is the preferred installation method. The overlay preserves the
exact modified files for inspection and recovery. Upstream middleware
licensing continues to apply to derived source files.
