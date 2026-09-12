# Fixed Tp=50 ms: Optimized Default vs CALM 4.0 and CALM 4.1

## Question

Does CALM suppress retransmission backlog and improve Reliable delivery when
the pacing period is fixed at `50 ms`?

## Controlled setup

Both runs used the same settings except for CALM.

- Middleware: Fast DDS 2.6.11, Reliable QoS
- Payload: 1,024 KiB
- Publish rate: 20 Hz, 2,000 samples
- Offered payload rate: 167.77 Mbps
- Loopback netem: 180 Mbps, `3 +/- 1 ms`, persistent packet loss 10%
- OPT 1: `maxMessageSize=1472 B`
- OPT 2: periodic HEARTBEAT period `25 ms`
- Timeout: 300 seconds
- Control: Optimized Default, CALM disabled
- Treatment: CALM 4.0 enabled, fixed `T_p=50 ms`, `K_d=K_i=0.25`

OPT 1 and OPT 2 are controlled variables. CALM is the manipulated variable.

## Results

| Metric | Optimized Default | CALM | Relative result |
| --- | ---: | ---: | ---: |
| Published | 2000 | 2000 | equal |
| Received | 1031 | 2000 | 969 more samples |
| Receive ratio | 51.55% | 100% | full recovery |
| Elapsed | 301.32 s, timeout | 232.70 s | CALM completed |
| Mean delay | 155.201 s | 73.883 s | 52.4% reduction |
| p95 delay | 245.546 s | 125.070 s | 49.1% reduction |
| Maximum delay | 246.636 s | 129.296 s | 47.6% reduction |
| Maximum `U` | 246,193,600 B | 30,833,192 B | 87.5% reduction |
| Maximum `U` | 234.79 MiB | 29.40 MiB | 205.38 MiB less |
| Retransmit count p50 | 8 | 2 | 75.0% reduction |
| Retransmit count p95 | 20 | 3 | 85.0% reduction |
| Retransmit count max | 37 | 8 | 78.4% reduction |
| Failed-repair count p50 | 7 | 1 | 85.7% reduction |
| Failed-repair count p95 | 19 | 2 | 89.5% reduction |
| Failed-repair count max | 36 | 7 | 80.6% reduction |
| Oldest repair age max | 83.338 s | 16.449 s | 80.3% reduction |
| ACK-progress stall max | 83.338 s | 4.530 s | 94.6% reduction |

The Default run stopped at the timeout, so its delay distribution contains
only the 1,031 samples received before termination. The missing 969 samples
would not lower that tail. The comparison therefore does not exaggerate
Default's completion performance.

## Interpretation

The strongest result is not only lower delay. CALM changed the delivery outcome
from timeout at 51.55% reception to complete 2,000/2,000 reception. At the same
time, maximum unacknowledged repair debt fell by 87.5%, and p95 repeated repair
attempts fell from 20 to 3.

This combination supports the intended mechanism:

1. Optimized Default released requested repair too aggressively under a
   near-capacity lossy workload.
2. Repeated repair occupied the path and ACK progress stalled for as long as
   83.34 seconds.
3. CALM limited each release with `B`, paced pending repair at 50 ms, and served
   the oldest repair first.
4. Smaller repair bursts reduced repeated failure, allowing cumulative ACK to
   advance and `U` to remain bounded at a much lower level.

The run demonstrates that the CALM control direction is useful even before a
general `T_p` equation is finalized.

## CALM 4.1 same-condition run

An additional CALM 4.1 run used the same nominal workload and fixed
`T_p=50 ms`. It was executed separately from the Default run, so this is a
cross-run comparison rather than a randomized or interleaved paired A/B trial.

| Metric | Optimized Default | CALM 4.1 | Relative result |
| --- | ---: | ---: | ---: |
| Published | 2000 | 2000 | equal |
| Received | 1031 | 2000 | 969 more samples |
| Receive ratio | 51.55% | 100% | full recovery |
| Elapsed | 301.32 s, timeout | 233.17 s | CALM 4.1 completed |
| Mean delay | 155.201 s | 77.731 s | 49.9% reduction |
| p95 delay | 245.546 s | 127.203 s | 48.2% reduction |
| Maximum delay | 246.636 s | 131.371 s | 46.7% reduction |
| Maximum `U` | 234.79 MiB | 34.45 MiB | 85.3% reduction |
| Retransmit count p50/p95/max | 8/20/37 | 2/4/9 | lower throughout |
| Failed-repair count p50/p95/max | 7/19/36 | 1/3/9 | lower throughout |
| Oldest repair age max | 83.338 s | 22.350 s | 73.2% reduction |
| ACK-progress stall max | 83.338 s | 5.182 s | 93.8% reduction |

Local source results:

- Default: `DDSOPT_loopback_fastdds_calm4_p1024_h20_per10_ab_20260821_223536`
- CALM 4.1: `DDSOPT_loopback_fastdds_calm41_tpsweep_p1024_h20_per10_tp50_20260907_003036`

This CALM 4.1 run used fixed pacing, not the experimental entry-ACK pacing
formula. It confirms the same qualitative direction as CALM 4.0, but its
cross-run percentages should not be presented as stronger evidence than the
CALM 4.0 paired result. A new interleaved Default/CALM 4.1 repeated experiment
is still required for a direct CALM 4.1 treatment-effect claim.

## Scope

This is a representative single-pair experiment under loopback/netem. It is
strong evidence for this workload, but not a universal performance claim.
Repeated trials across payload size, publish rate, transient loss, disconnect,
multiple Writers and physical Wi-Fi links remain necessary.

The general DDS-local method for choosing `T_p` is still in progress. Current
results show that fixed 50 ms works much better than Optimized Default in this
condition, not that 50 ms is optimal for every workload.
