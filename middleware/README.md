# Middleware source snapshots

This directory stores research patches and source overlays for the middleware
versions used by CALM.

- `fastdds-v2.6.11`: based on eProsima Fast DDS tag `v2.6.11`, commit
  `87dd60c8f3e8694481ad0279bd4cc8c645050da3`
- `cyclonedds-0.10.5`: based on Eclipse Cyclone DDS tag `0.10.5`, commit
  `2cdd114cbd18340c606573b4cc8dc20cc161ec5a`

The patch is the preferred way to apply CALM. The overlay preserves the exact
modified files for inspection and recovery. Upstream middleware licensing
continues to apply to derived source files.
