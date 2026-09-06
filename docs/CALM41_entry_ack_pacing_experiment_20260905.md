# CALM 4.1 entry-ACK pacing experiment

## Change

CALM 4.1 now supports two episode-fixed pacing modes.

- `fixed`: use `FASTDDS_CALM_PACING_MS` as before.
- `entry_ack`: at CALM entry, calculate
  `T_p = eta * B_initial * 8 / (mu_entry * 1000)` in milliseconds, where
  `mu_entry` is the Writer-side cumulative-ACK goodput in Mbps.

The calculated period remains fixed during that CALM episode. If no ACK rate
exists at entry, the implementation explicitly falls back to the configured
fixed period. The mode is selected with `FASTDDS_CALM41_PACING_MODE`, and eta
with `FASTDDS_CALM41_PACING_ETA`.

## Validation condition

- Fast DDS Reliable, OPT 1 and 2
- 1,024 KiB at 20 Hz, 2,000 samples
- loopback netem: 180 Mbps, 3 +/- 1 ms
- 200-sample loss-free warm-up, then persistent packet loss 10%
- `K_dec=K_inc=0.25`, timeout 300 s
- `eta=1.10`

## Result

| Mode | Received | Elapsed | Mean delay | p95 delay | Max U | Retry p95/max |
|---|---:|---:|---:|---:|---:|---:|
| Fixed 50 ms | 2,000/2,000 | 266.33 s | 110.90 s | 160.93 s | 53.99 MB | 5/15 |
| Entry-ACK formula | 1,322/2,000 | 300 s timeout | 122.69 s | 220.37 s | 27.19 MB | 4/9 |

The formula path used `mu_entry=50.182 Mbps` and calculated
`T_p=183.890 ms`. The measured median interval between release events was
184.54 ms, confirming that the calculated period controlled actual pacing.

The calculation reduced peak repair debt and repeated repair attempts, but it
under-released traffic and failed the complete-reception requirement. In this
near-capacity fragmented workload, cumulative sample ACK goodput substantially
underestimated usable packet service capacity. Therefore `entry_ack` remains
experimental and `fixed` remains the default mode.

With packet loss active from startup, no pre-entry ACK estimate existed, so
the implementation correctly used the 50 ms fallback. A pure entry-rate
formula needs a separate, defensible cold-start policy before it can replace
fixed pacing.

## Result directories

- Fixed warm-up control:
  `DDSOPT_loopback_fastdds_calm41_fixed50_warmup_p1024_h20_a10_20260905_230440`
- Formula warm-up validation:
  `DDSOPT_loopback_fastdds_calm41_entry_ack_v3_warmup_p1024_h20_a10_20260905_232257`
- Cold-start fallback check:
  `DDSOPT_loopback_fastdds_calm41_entry_ack_cold_p1024_h20_per10_20260905_225822`

