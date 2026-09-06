# Equal nominal B/Tp pacing experiment

## Purpose

This experiment asks whether splitting the same nominal release rate into
smaller, more frequent batches improves CALM recovery.

## Setup

- Fast DDS 2.6.11 Reliable QoS
- OPT 1: `maxMessageSize=1472 B`
- OPT 2: HEARTBEAT period `25 ms`
- Payload `1 MiB`, publish rate `20 Hz`, 2,000 samples
- Loopback netem: 180 Mbps, `3 +/- 1 ms`, persistent PER 10%
- `B_min=B_initial=B_max`
- `K_d=K_i=0`
- Three repetitions in interleaved order: A-B-C / B-C-A / C-A-B

## Results

| Case | B | Tp | Nominal rate | Complete | Mean elapsed | Mean delay | Mean p95 | Delay std | Mean max U |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| A | 1 MiB | 50 ms | 167.77 Mbps | 3/3 | 295.51 s | 101.83 s | 185.24 s | 53.67 s | 28.64 MiB |
| B | 512 KiB | 25 ms | 167.77 Mbps | 3/3 | 282.97 s | 106.41 s | 174.69 s | 46.20 s | 36.24 MiB |
| C | 256 KiB | 12.5 ms | 167.77 Mbps | 0/3 | timeout | 148.88 s* | 214.39 s* | incomplete | 41.33 MiB |

`*` Timeout runs include delay only for samples received before termination.

The measured pacing-only release rates were approximately 99.2, 77.4 and
50.9 Mbps for A, B and C. Equal nominal `B/Tp` did not produce equal realized
service rates because the current scheduler is not work-conserving across
empty/overlapping ticks, and smaller batches delay completion of a fragmented
1 MiB sample.

## Conclusion

Moderate splitting (B) improved tail delay and jitter relative to A, but did
not reduce every metric. Aggressive splitting (C) failed all three runs. The
result supports a non-monotonic batch-size trade-off, not the claim that a
shorter `Tp` is always better.
