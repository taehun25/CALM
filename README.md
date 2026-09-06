# CALM: Congestion-Aware Loss-Recovery Modulation for ROS 2 DDS

CALM은 ROS 2 DDS의 `RELIABLE` QoS에서 손실 복구 트래픽이 큰 burst로
release되면서 지연과 추가 손실을 증폭시키는 문제를 줄이기 위한 writer-side
재전송 제어 연구입니다.

현재 저장소에는 다음 연구 프로토타입이 포함되어 있습니다.

- Fast DDS 2.6.11 writer retransmission path의 CALM 4.1 구현
- Cyclone DDS 0.10.5 writer retransmission path의 CALM 3 계열 포팅
- Reader별 backlog, ACK progress, sample/fragment retry 계측 CSV
- OPT 1, 2 profile 생성과 loopback/Wi-Fi 실험 자동화
- Default DDS와 CALM을 비교하는 ROS 2 publisher/subscriber

> 현재 상태: CALM의 진입 조건, oldest-first 처리, held-new, budget 제어와
> pacing 실행 경로는 구현되어 있습니다. 그러나 pacing period `T_p`를 다양한
> DDS workload와 Wi-Fi 링크에서 일반화하는 식은 아직 연구 중입니다. 현재
> 결과를 완성된 제품이나 모든 네트워크에 대한 최적 설정으로 해석하면 안 됩니다.

## Problem

Reliable RTPS의 기본 손실 복구 과정은 다음과 같습니다.

```text
Writer                                  Reader
  | --- DATA / DATA_FRAG ----------------> |
  | --- HEARTBEAT ------------------------> |
  | <--- ACKNACK / NACKFRAG --------------- |
  | --- requested repair burst ----------> |
  | <--- cumulative ACK progress ---------- |
```

무선 손실이나 단절 복구 중에는 Reader가 요청한 repair가 한꺼번에 release될 수
있습니다. repair burst가 링크와 송신 큐를 압박하면 추가 손실, 반복 NACK,
더 큰 repair burst가 이어질 수 있습니다. 완전한 storm까지 도달하지 않아도
수십 초 이상의 tail delay와 낮은 실제 수신 Hz가 나타날 수 있습니다.

CALM은 RTPS packet format이나 Reader를 변경하지 않고 Writer의 DDS 상태만으로
이 loop를 제어합니다.

## Optimized Baseline

CALM 평가에서는 다음 두 설정을 Default와 CALM 양쪽에 동일하게 적용합니다.
따라서 OPT 1, 2는 통제 변인이고 CALM만 조작 변인입니다.

- **OPT 1:** `maxMessageSize=1472 B`
- **OPT 2:** periodic HEARTBEAT period를 publish period의 절반으로 설정
- Piggyback HEARTBEAT는 활성 상태 유지

CALM은 링크 용량을 미리 입력하는 정적 link-capacity optimization은 사용하지
않습니다.

## Observed State

CALM은 Writer의 ReaderProxy 또는 그에 대응하는 per-reader 상태를 관측합니다.

- `U_r`: Reader `r`이 한 번 이상 NACK했지만 cumulative ACK으로 해소되지 않은
  serialized sample byte
- `Delta U_n`: 연속된 valid feedback round 사이의 `U` 변화
- `F_r^old`: 현재 가장 오래된 repair sample이 실제로 재전송된 후 다시 요청된
  횟수
- ACK base/high sequence와 ACK progress 여부
- repair release 시각과 이후 feedback 도착 시각
- feedback RTT EWMA
- sample별 실제 retransmission count와 failed-repair count
- pending requested byte, scheduled repair byte, held-new byte

`F`는 단순히 NACKFRAG submessage가 여러 번 도착했다고 증가하지 않습니다.
실제로 전송한 repair 영역과 이후 valid feedback의 요청 영역이 겹칠 때만
실패한 repair round로 계산합니다.

## CALM 4.1 Controller

CALM 4.1은 가장 오래된 sample의 failed-repair count가 1 이상이면 해당
Reader에 대해 활성화됩니다.

```text
CALM_ACTIVE := F_old >= 1
NORMAL      := U == 0
```

각 valid feedback round `n`에서 다음 신호를 사용합니다.

```text
D_n := Delta F_old >= 1 AND (Delta U_n >= 0 OR T_ACK >= T_to)
I_n := ACK repair progress AND Delta U_n < 0
```

Budget은 다음 AIMD 형태로 갱신됩니다.

```text
              max(B_min, B_n * (1 - K_d * p_n)),  D_n
B_(n+1) =     min(B_max, B_n + K_i * q_n * S_bar), I_n
              B_n,                                  otherwise
```

- `p_n`: 실제 전송 repair 중 다시 요청된 비율
- `q_n`: 해당 round에서 ACK으로 복구된 비율
- `S_bar`: ReaderProxy가 관리한 serialized sample의 평균 크기
- `T_to = 4 * T_FB_hat`
- feedback RTT를 아직 측정하지 못했다면 HEARTBEAT period를 초기 reference로 사용

`D_n`은 반복 실패가 확인되고 backlog가 줄지 않거나 ACK가 정체될 때 budget을
곱셈 감소합니다. `I_n`은 repair ACK progress와 backlog 감소가 동시에 확인될 때
sample 크기에 비례해 budget을 가산 증가합니다.

## Scheduling

CALM_ACTIVE 상태에서 각 pacing opportunity는 총 budget `B`를 다음 순서로
사용합니다.

```text
b_repair = min(pending_oldest_repair, B)
b_new    = min(held_new, B - b_repair)
b_repair + b_new <= B
```

- 가장 오래된 requested repair부터 release합니다.
- repair가 budget을 모두 사용하면 새 전송은 WHC의 held-new 상태로 기다립니다.
- repair가 budget보다 작으면 남은 budget으로 held-new를 release합니다.
- application `write()`를 중단하거나 sample을 의도적으로 drop하지 않습니다.
- 남은 repair는 다음 HEARTBEAT까지 기다리지 않고 pacing opportunity마다 처리합니다.
- 이전 batch가 DDS 송신 경로에 남아 있으면 다음 release를 건너뜁니다.
- `U=0`이고 held-new가 해소되면 NORMAL 경로로 복귀합니다.

## Pacing Period Status

`T_p`는 아직 최종 결정되지 않았습니다.

현재 Fast DDS 구현은 다음 모드를 지원합니다.

- `fixed`: `FASTDDS_CALM_PACING_MS`를 CALM episode 동안 고정 사용
- `entry_ack`: 진입 시 Writer가 관측한 ACK goodput으로 한 번 계산

실험적 계산식은 다음과 같습니다.

```text
T_p = eta * 8 * B_initial / mu_entry
```

그러나 `mu_entry`가 sample-level cumulative ACK goodput을 과소평가한 실험에서
`T_p=183.89 ms`가 계산되어 under-release와 timeout이 발생했습니다. 따라서
`entry_ack`는 실험용이며 기본 모드는 `fixed`입니다.

### Fixed-period observations

`1 MiB x 20 Hz`, 180 Mbps loopback, `3 +/- 1 ms`, persistent PER 10%에서
adaptive CALM의 고정 period를 비교했습니다.

| T_p | Received | Elapsed | p95 delay | Max U |
| ---: | ---: | ---: | ---: | ---: |
| 12.5 ms | 2000/2000 | 252.27 s | 144.03 s | 32.19 MiB |
| 25 ms | 2000/2000 | 219.15 s | 110.42 s | 34.58 MiB |
| 50 ms | 2000/2000 | 233.17 s | 127.20 s | 34.45 MiB |

이 결과는 `T_p`가 짧을수록 항상 좋다는 가설을 지지하지 않습니다.

### Equal nominal B/T_p experiment

Pacing granularity를 분리하기 위해 다음 세 설정에서
`B_min=B_initial=B_max`, `K_d=K_i=0`으로 각 3회 반복했습니다.

| Case | Fixed B | Fixed T_p | Nominal B/T_p | Complete runs | Mean p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| A | 1 MiB | 50 ms | 167.77 Mbps | 3/3 | 185.24 s |
| B | 512 KiB | 25 ms | 167.77 Mbps | 3/3 | 174.69 s |
| C | 256 KiB | 12.5 ms | 167.77 Mbps | 0/3 | 214.39 s* |

`*` Case C의 p95는 timeout 전에 도착한 sample만 포함합니다.

Case B는 A보다 p95와 jitter를 줄였지만 평균 `U`는 증가했습니다. Case C는 세
번 모두 timeout이었습니다. 또한 실제 pacing release rate는 A/B/C 각각 약
99.2/77.4/50.9 Mbps로 달랐습니다. 같은 명목 `B/T_p`라도 queue non-overlap,
빈 budget, fragment 처리 비용과 sample completion 때문에 같은 실제 service
rate를 보장하지 않는다는 점을 확인했습니다.

따라서 현재 연구의 주요 미완성 항목은 다음과 같습니다.

1. 동일한 실제 service rate에서 burst cap만 비교하는 work-conserving 실험
2. sample completion을 지나치게 늦추지 않는 `B` 하한
3. DDS-local 관측만으로 일반화 가능한 `T_p` 결정식
4. 다양한 payload/rate 및 실제 Wi-Fi에서 반복 검증

## Representative Fast DDS Result

동일한 OPT 1, 2 조건의 Default와 CALM을 비교한 대표 loopback 결과입니다.

- Workload: `1 MiB x 20 Hz`, 2,000 samples
- Link: 180 Mbps, `3 +/- 1 ms`, persistent PER 10%
- Default: `1031/2000`, 300 s timeout, p95 245.55 s, max `U` 246.19 MB
- CALM fixed 50 ms: `2000/2000`, 232.70 s, p95 125.07 s,
  max `U` 30.83 MB

이 결과는 CALM의 budget pacing이 대량 repair burst와 backlog peak를 줄일 수
있음을 보여줍니다. 단일 조건의 결과이므로 모든 Wi-Fi에서 같은 개선률을
보장하지는 않습니다.

## Cyclone DDS Status

Cyclone DDS 0.10.5에도 per-proxy-reader 관측, retransmit enqueue 제어,
oldest-first pacing과 held-new를 구현했습니다. 표준 8개 일시 손실/혼잡 조건에서
Default와 CALM 모두 2,220/2,220 sample을 수신했습니다.

- `lambda < mu` 5조건 평균 p95: 185.1 ms -> 118.2 ms
- `lambda >= mu` 3조건 평균 p95: 490.2 ms -> 287.9 ms
- 일부 조건에서는 jitter와 수신 Hz가 소폭 악화
- 지속적인 `4 MiB x 10 Hz + PER 20%` 과부하에서는 과감쇠로 Default보다 악화

Cyclone 포팅은 기능 프로토타입이며 Fast DDS CALM 4.1과 완전히 동일한 최종
controller로 통합하는 작업은 남아 있습니다.

## Repository Layout

```text
middleware/
  fastdds-v2.6.11/
    CALM.patch
    overlay/
  cyclonedds-0.10.5/
    CALM.patch
    overlay/
experiments/current/calm_pretest_yw/
docs/
S1/
document/CALM_proposal.md
```

- `CALM.patch`: upstream DDS source에 적용할 전체 patch
- `overlay`: 현재 수정 파일을 경로 그대로 보존한 snapshot
- `calm_pretest_yw`: ROS 2 nodes, XML profile generator와 자동화 스크립트
- `docs`: 구현 및 실험 보고서
- `S1`: 초기 baseline experiment snapshot

대용량 raw CSV와 ROS 2 `build/install/log` 산출물은 저장소에 포함하지 않습니다.

## Apply Middleware Patches

### Fast DDS 2.6.11

```bash
git clone --branch v2.6.11 https://github.com/eProsima/Fast-DDS.git Fast-DDS
cd Fast-DDS
git apply /path/to/CALM/middleware/fastdds-v2.6.11/CALM.patch
```

### Cyclone DDS 0.10.5

```bash
git clone --branch 0.10.5 https://github.com/eclipse-cyclonedds/cyclonedds.git CycloneDDS
cd CycloneDDS
git apply /path/to/CALM/middleware/cyclonedds-0.10.5/CALM.patch
```

ROS 2 Humble workspace에 `experiments/current/calm_pretest_yw`를 복사한 뒤 필요한
RMW package까지 `colcon build --symlink-install`로 빌드합니다.

## Loopback Example

```bash
source /opt/ros/humble/setup.bash
source ~/ros2_ws/install/setup.bash
sudo -v

python3 ~/ros2_ws/src/calm_pretest_yw/scripts/ddsopt_loopback_automation.py \
  --dds fastdds \
  --label calm41_example \
  --modes opt12 \
  --controls default,calm \
  --scenarios normal \
  --payload-kb 1024 \
  --hz 20 \
  --count 2000 \
  --baseline-loss-percent 10 \
  --link-rate-mbit 180 \
  --delay-ms 3 \
  --jitter-ms 1 \
  --timeout-s 300 \
  --calm-controller calm41 \
  --calm41-pacing-mode fixed \
  --calm-pacing-ms 50 \
  --calm4-k-dec 0.25 \
  --calm4-k-inc 0.25
```

자동화는 종료 시 loopback qdisc 제거를 시도합니다. 중단 후에는 다음 명령으로
직접 확인할 수 있습니다.

```bash
sudo tc qdisc del dev lo root 2>/dev/null || true
tc qdisc show dev lo
```

## CSV Metrics

`summary.csv`의 주요 항목은 다음과 같습니다.

- `received_count`, `timed_out`, `elapsed_s`
- `sub_delay_mean_ms`, `sub_delay_p95_ms`, `sub_delay_std_ms`
- `sub_actual_hz`, `pub_actual_hz`
- `rho_max_bytes`, `rho_final_bytes` (`rho`는 현재 연구의 `U`와 같은 관측값)
- `sample_retransmit_count_p95/max`
- `sample_failed_repair_count_p95/max`
- `calm_active_rows`, `calm_budget_min/max_bytes`

Event-level `fastdds_storm_*.csv`에는 sample sequence, repair 상태, ACK progress,
failed repair, `Delta U`, budget과 pacing period가 기록됩니다.

## Limitations and Next Work

- `T_p` 일반식과 cold-start service-rate estimator 미완성
- 실제 Wi-Fi에서 여러 AP, RSSI, 간섭 조건의 반복 검증 필요
- 다중 Writer/Reader 간 fairness와 reader isolation 검증 필요
- 지속적인 `lambda > mu`에서 source admission 또는 overload policy 필요
- timeout run에서도 일관된 jitter/receive-Hz 통계를 남기도록 계측 개선 필요
- Cyclone DDS를 CALM 4.1 식과 완전히 통합하고 동일 matrix 재검증 필요

CALM은 현재 연구용 prototype입니다. 실시간 또는 안전 필수 시스템에 적용하기
전에 workload별 검증이 필요합니다.

## Target Venue

- ACM/IEEE International Conference on Cyber-Physical Systems (ICCPS)
