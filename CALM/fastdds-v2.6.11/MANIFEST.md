# CALM 4.3 source manifest

- Upstream: eProsima Fast DDS
- Tag: `v2.6.11`
- Commit: `87dd60c8f3e8694481ad0279bd4cc8c645050da3`
- Controller selector: `FASTDDS_CALM_CONTROLLER=calm43`

The patch and overlay contain these modified upstream paths:

```text
include/fastdds/rtps/writer/ChangeForReader.h
include/fastdds/rtps/writer/ReaderProxy.h
include/fastdds/rtps/writer/StatefulWriter.h
src/cpp/rtps/writer/RTPSWriter.cpp
src/cpp/rtps/writer/ReaderProxy.cpp
src/cpp/rtps/writer/StatefulWriter.cpp
test/mock/rtps/StatefulWriter/fastdds/rtps/writer/StatefulWriter.h
test/unittest/rtps/writer/ReaderProxyTests.cpp
```

`CALM.patch` was generated from the workspace Fast DDS checkout against the
upstream commit above. SHA-256 checksums are stored in `SHA256SUMS` and can be
verified from this directory with `sha256sum -c SHA256SUMS`.
