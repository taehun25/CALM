# CALM: Congestion-Aware Loss-Recovery Modulation for ROS 2 DDS

CALM은 ROS 2 DDS의 `RELIABLE` QoS에서 대량 repair가 한꺼번에 release되어
추가 손실, 반복 NACK, 긴 delay를 만드는 positive feedback 문제를 줄이기 위한 
재전송 제어 연구입니다. RTPS format이나 Reader를 바꾸지 않고 Writer가
DDS layer에서 관측할 수 있는 정보만 사용합니다.

> **현재 상태:** Fast DDS 2.6.11에 CALM 4.1의 진입 조건, budget 제어,
> oldest-repair-first scheduling, held-new, pacing 실행 경로가 구현되어 있습니다.
> 고정 `T_p=50 ms`의 CALM 4.0 paired 실험에서는 Optimized Default보다 크게
> 개선됐고 CALM 4.1의 별도 실행에서도 같은 방향이 확인됐지만,
> workload와 링크가 달라져도 적용할 수 있는 `T_p` 결정식은 **아직 연구 중**입니다.
> Cyclone DDS 0.10.5 포팅은 CALM 3 계열 기능 prototype입니다.

## Background

Reliable RTPS의 기본 손실 복구 흐름은 다음과 같습니다.

```text
Writer                                       Reader
  | --- DATA / DATA_FRAG -------------------->  |
  | --- HEARTBEAT ----------------------------> |
  | <--- ACKNACK / NACKFRAG ------------------- |
  | --- requested repair -------------------->  |
  | <--- cumulative ACK progress -------------- |
```

무선 손실이나 단절 복구 중 Reader가 요청한 repair가 큰 burst로 release되면 그 자체가 대역폭을 점유하여 링크와
송신 큐가 다시 혼잡해질 수 있습니다. 그러면 repair 자체가 재손실되고 같은 영역이
다시 NACK되어, WHC backlog와 지연이 함께 증가합니다. 
<!--완전한 storm에 이르지 않더라도 실제 수신 Hz와 실시간성이 크게 저하될 수 있습니다.-->

### Optimized baseline

실험에서는 다음 두 최적화를 Default와 CALM 양쪽에 똑같이 적용합니다.
해당 최적화는
[Optimizing ROS 2 Communication for Wireless Robotic Systems](docs/Optimizing_ROS_2_Communication_for_Wireless_Robotic_Systems.pdf)을
참고하였습니다.
<!--따라서 OPT 1, 2는 통제 변인이고 CALM controller만 조작 변인입니다.-->

- **OPT 1:** `maxMessageSize=1472 B`
- **OPT 2:** periodic HEARTBEAT 주기를 publish period의 절반으로 설정
<!-- - Piggyback HEARTBEAT는 기존처럼 활성화-->

<!--링크 용량을 미리 입력하는 정적 link-capacity optimization은 사용하지 않습니다.-->

## Notation

### Observation and feedback

| Symbol | Name | Meaning | Unit |
| --- | --- | --- | --- |
| $r$ | Reader index | Writer와 match된 Reader 또는 ReaderProxy 식별자 | - |
| $n$ | Feedback round | 실제 repair release 뒤 유효 ACKNACK/NACKFRAG가 도착해 닫힌 제어 round | round |
| $U_{r,n}$ | Repair debt | Reader $r$이 NACK한 뒤 cumulative ACK으로 아직 해소되지 않은 WHC sample 총량 | byte |
| $\Delta U_n$ | Debt change | $U_n-U_{n-1}$; 양수/0이면 적체 비감소, 음수이면 repair debt 감소 | byte |
| $F^{old}_{r,n}$ | Oldest failed-repair count | 가장 오래된 repair sample에서 실제 전송 영역이 이후 feedback에 다시 요청된 누적 횟수 | count |
| $\Delta F^{old}_{r,n}$ | New repair failure | 현재 round에 새로 확인된 oldest sample의 repair 실패 횟수 | count |
| $p_n$ | Failure fraction | 현재 round에서 release한 repair 중 다시 요청된 byte 비율, $0\le p_n\le1$ | ratio |
| $q_n$ | Recovery fraction | 현재 round에서 ACK으로 해소된 repair byte 비율, $0\le q_n\le1$ | ratio |
| $\bar S$ | Mean sample size | ReaderProxy가 관리한 serialized sample byte의 평균 | byte/sample |
| $D_n$ | Decrease event | 반복 실패와 debt 비감소 또는 ACK timeout이 함께 확인된 상태 | Boolean |
| $I_n$ | Increase event | ACK repair progress와 debt 감소가 함께 확인된 상태 | Boolean |

### Budget and scheduling

| Symbol | Name | Meaning | Unit |
| --- | --- | --- | --- |
| $B_n$ | Release budget | round $n$에서 한 pacing opportunity에 허용하는 repair와 held-new의 총량 | byte |
| $B_{initial}$ | Initial budget | CALM episode 진입 직후 사용하는 첫 budget | byte |
| $B_{min}$ | Minimum budget | 과도한 감소로 전송이 멈추지 않도록 하는 budget 하한 | byte |
| $B_{max}$ | Maximum budget | 반복 증가가 다시 큰 burst를 만들지 않도록 하는 budget 상한 | byte |
| $K_d$ | Decrease gain | 실패 비율 $p_n$이 budget 감소에 미치는 크기 | - |
| $K_i$ | Increase gain | 복구 비율 $q_n\bar S$가 budget 증가에 미치는 크기 | - |
| $R$ | Pending repair | 현재 release를 기다리는 requested repair 총량 | byte |
| $H$ | Held-new | WHC에는 들어왔지만 CALM이 network release를 보류한 새 데이터 총량 | byte |
| $b_r$ | Repair allocation | 한 pacing opportunity에서 repair에 배정된 budget | byte |
| $b_n$ | New-data allocation | repair 배정 후 남은 budget 중 held-new에 배정된 양 | byte |

### Time and rate

| Symbol | Name | Meaning | Unit |
| --- | --- | --- | --- |
| $T_p$ | Pacing period | budget batch를 release한 뒤 다음 opportunity까지의 간격 | ms |
| $T_{ACK,n}$ | ACK-progress age | 마지막 cumulative ACK base 진전 이후 지난 시간 | ms |
| $T_{to,n}$ | Feedback timeout | ACK 정체를 판단하는 동적 timeout | ms |
| $\widehat T_{FB,n}$ | Feedback-time estimate | repair release부터 이에 대응하는 valid feedback까지 시간의 추정값 | ms |
| $\widehat\mu_{entry}$ | Entry service-rate estimate | CALM 진입 전 cumulative ACK로 확인한 DDS delivery service rate | bit/s |
| $\eta$ | Safety factor | 추정 오차와 처리 변동을 고려해 계산한 $T_p$에 주는 여유 계수 | - |
| $\lambda$ | Offered rate | application이 Writer에 공급하는 serialized payload rate | bit/s |
| $\mu$ | Service rate | DDS와 경로가 실제 ACK 완료까지 처리할 수 있는 rate | bit/s |

`feedback round`는 단순 HEARTBEAT 횟수가 아닙니다. 실제 repair를 release한 뒤
그 영역에 대응하는 유효 feedback을 Writer가 받아 성공 또는 실패를 판정할 때
한 round가 닫힙니다.

## 1. Budget B Control

`B`는 한 번의 pacing에서 release할 수 있는 **재전송과 새전송의
합계 byte budget**입니다. CALM 4.1은 Writer의 ReaderProxy별로 다음 값을
관측합니다.

- $U_r$: Reader $r$이 한 번 이상 NACK했지만 cumulative ACK으로 해소되지 않은
  serialized sample byte
- $\Delta U_n$: 연속된 valid feedback round 사이의 $U$ 변화
- $F^{old}_{r,n}$: 현재 가장 오래된 repair sample이 실제 재전송된 뒤 다시
  요청된 횟수
- ACK base/high sequence와 ACK repair progress
- repair release와 feedback 도착 시각, feedback RTT EWMA
- sample별 retransmission count와 failed-repair count
- pending repair, scheduled repair, held-new byte

<!--`F`는 NACKFRAG submessage가 단순히 여러 번 도착했다고 증가하지 않습니다.
실제로 보낸 repair 영역과 이후 valid feedback의 요청 영역이 겹칠 때만 실패한
repair round로 인정합니다.-->
`F`는 실제로 보낸 재전송이 실패하여 NACK을 받은 경우에만 실패한
repair round로 인정합니다.

### Activation and return

가장 오래된 sample의 repair 실패가 확인되면 해당 Reader의 CALM episode가
시작되고, repair debt가 모두 해소되면 기존 DDS 경로로 돌아갑니다.

$$
\mathrm{CALM\_ACTIVE} \iff F^{old}_{r,n} \ge 1
$$

$$
\mathrm{NORMAL} \iff U_n = 0
$$

### Decrease and increase

각 valid feedback round $n$에서 감소 조건과 증가 조건을 다음처럼 정의합니다.

$$
D_n = \left(\Delta F^{old}_{r,n} \ge 1\right)
\land \left(\Delta U_n \ge 0 \lor T_{ACK,n} \ge T_{to,n}\right)
$$

$$
I_n = \mathrm{ACK\ repair\ progress} \land \Delta U_n < 0
$$

현재 timeout reference는 측정한 feedback time의 4배이며($ T_{to,n} = 4\widehat{T}_{FB,n} $), 초기 feedback 측정값이
없으면 HEARTBEAT period를 사용합니다.

<!-- $$
T_{to,n} = 4\widehat{T}_{FB,n}
$$ -->

Budget은 다음 AIMD 계열 식으로 feedback round마다 갱신됩니다.

$$
B_{n+1} =
\begin{cases}
\max\left(B_{min}, B_n(1-K_d p_n)\right), & D_n \\
\min\left(B_{max}, B_n+K_i q_n\bar{S}\right), & I_n \\
B_n, & \text{otherwise}
\end{cases}
$$

- $p_n$: 실제 전송한 repair 중 다음 feedback에서 다시 요청된 비율
- $q_n$: 해당 round에서 ACK으로 복구된 비율
- $\bar{S}$: ReaderProxy가 관리한 serialized sample의 평균 크기
- $K_d$, $K_i$: 감소 및 증가 gain

반복 실패와 backlog 비감소가 함께 확인되면 현재 `B`에 비례해 빠르게 감소하고,
실제 ACK repair progress와 backlog 감소가 확인되면 sample 규모에 비례해
천천히 증가합니다.

<!-- ### Repair-first scheduling

각 pacing opportunity에서 pending repair $R$과 held-new $H$에 budget을 다음
순서로 배분합니다.

$$
b_r = \min(R, B), \qquad
b_n = \min(H, B-b_r), \qquad
b_r+b_n \le B
$$

1. 가장 오래된 requested repair부터 `b_r`만큼 release합니다.
2. repair가 `B`보다 작을 때만 남은 budget으로 held-new를 release합니다.
3. application `write()`는 막지 않고 새 Change를 WHC에 보관하되 network release를
   잠시 보류합니다.
4. 남은 repair는 다음 HEARTBEAT를 기다리지 않고 pacing opportunity마다 처리합니다.
5. 이전 batch가 DDS 송신 경로에 남아 있으면 다음 tick의 추가 등록을 건너뛰어
   여러 batch가 다시 하나의 burst로 합쳐지는 것을 막습니다. -->

## 2. Pacing Period Tp 구현 중

$T_p$는 CALM이 한 번의 budget `B`를 release한 뒤 다음 release opportunity까지
기다리는 시간입니다. CALM episode 진입 시 한 번 정하고 episode 동안 고정하는
것이 현재 설계 방향입니다.

### Current implementation

- `fixed`: `FASTDDS_CALM_PACING_MS`를 episode 동안 고정 사용
- `entry_ack`: 진입 직전 ACK goodput으로 한 번 계산하는 실험 모드

`entry_ack`에서 시험한 식은 다음과 같습니다.

$$
T_p = \eta\frac{8B_{initial}}{\widehat{\mu}_{entry}}
$$

$\widehat{\mu}_{entry}$가 sample-level cumulative ACK goodput을 과소평가한
실험에서는 $T_p=183.89\,\mathrm{ms}$가 계산되어 under-release와 timeout이
발생했습니다. 따라서 현재 기본 검증값은 `fixed T_p=50 ms`이며,
`entry_ack`는 실험용입니다.

### What remains

`T_p`가 너무 짧으면 이전 batch가 처리되기 전에 다음 batch가 등록되고, 너무 길면
링크가 비어 있어도 repair를 release하지 않아 처리량과 sample completion이
나빠집니다. 다음 조건을 동시에 만족하는 DDS-local 결정식이 남은 핵심 과제입니다.

- 이전 batch와 겹치지 않을 만큼 길 것
- 한 feedback round 안에 여러 번 pacing할 수 있을 만큼 짧을 것
- payload, publish rate, 실제 Wi-Fi 변화에 일반화될 것
- sample completion과 ACK clock을 지나치게 늦추지 않을 것

동일한 명목 release rate $B/T_p=167.77\,\mathrm{Mbps}$로 3회씩 비교한 결과도
실제 release rate가 A/B/C 각각 99.2/77.4/50.9 Mbps로 달랐습니다.

| Case | Fixed B | Fixed T_p | Complete runs | Mean p95 delay |
| --- | ---: | ---: | ---: | ---: |
| A | 1 MiB | 50 ms | 3/3 | 185.24 s |
| B | 512 KiB | 25 ms | 3/3 | 174.69 s |
| C | 256 KiB | 12.5 ms | 0/3 | 214.39 s* |

`*` Case C는 timeout 전에 도착한 sample만 포함합니다. 이 결과는 같은 명목
$B/T_p$라도 queue non-overlap, 빈 budget, fragment 처리 비용과 sample
completion 때문에 같은 실제 service rate가 보장되지 않음을 보여줍니다.

## 3. Representative Result

Fast DDS에서 OPT 1, 2를 동일하게 적용한 Default와 CALM 4.0의 대표 paired A/B
결과.
<!-- 현재 핵심 구현인 CALM 4.1 이전의 controller 결과이므로 버전을
구분해서 해석해야 합니다.-->

- Workload: `1 MiB x 20 Hz`, 2,000 samples
- Loopback: 180 Mbps, <!--`3 +/- 1 ms`,--> persistent PER 10%
- OPT 1: `1472 B`, OPT 2 HEARTBEAT: `25 ms`
- CALM 4.0: fixed `T_p=50 ms`, `K_d=K_i=0.25`
- Timeout: 300 s

| Metric | Optimized Default | CALM 4.0 | Change |
| --- | ---: | ---: | ---: |
| Received | 1031/2000 | 2000/2000 | complete recovery |
| Timeout | yes | no, 232.70 s | timeout removed |
| Mean end-to-end delay | 155.20 s | 73.88 s | 52.4% lower |
| p95 end-to-end delay | 245.55 s | 125.07 s | 49.1% lower |
| Maximum $U$ | 234.79 MiB | 29.40 MiB | 87.5% lower |
| Retransmit count p95 | 20 | 3 | 85.0% lower |
| Failed-repair count p95 | 19 | 2 | 89.5% lower |
| Oldest repair age max | 83.34 s | 16.45 s | 80.3% lower |
| ACK-progress stall max | 83.34 s | 4.53 s | 94.6% lower |

이는 고정 `T_p=50 ms`에서 budget pacing이 큰 repair burst와 WHC peak를 줄이고
완전 수신을 회복한 결과입니다. <!--하나의 loopback/netem 조건에서 얻은 대표
1회 A/B이므로 모든 Wi-Fi에서 같은 개선률을 보장하지는 않습니다.--> 자세한 분석은
[`docs/fixed_tp50_default_vs_calm.md`](docs/fixed_tp50_default_vs_calm.md)에 있습니다.

<!-- Fast DDS 대표 Default는 timeout 전에 받은 1,031개 sample만으로 p95가
계산됐으므로 완전 수신 분포와 동등한 통계로 과해석하면 안 됩니다.

CALM 4.1은 같은 명목 조건의 별도 fixed-50 ms 실행에서 2,000/2,000 수신,
p95 127.20 s, maximum $U$ 34.45 MiB를 기록했습니다. 이는 CALM 4.0 paired
결과와 개선 방향은 같지만, 다른 시각에 수행한 실행이므로 위 paired 개선율 계산에는
포함하지 않았습니다.-->

## Implementation

### Fast DDS 2.6.11: CALM 4.1

| File | Responsibility |
| --- | --- |
| [`ReaderProxy.h`](CALM/fastdds-v2.6.11/overlay/include/fastdds/rtps/writer/ReaderProxy.h) | Reader별 $U$, retry, feedback, budget, held-new와 pacing state |
| [`ChangeForReader.h`](CALM/fastdds-v2.6.11/overlay/include/fastdds/rtps/writer/ChangeForReader.h) | sample/fragment별 repair scope와 실제 재실패 판정 |
| [`ReaderProxy.cpp`](CALM/fastdds-v2.6.11/overlay/src/cpp/rtps/writer/ReaderProxy.cpp) | CALM 진입/종료, $D_n$/$I_n$, B 증가·감소, timeout, held-new release |
| [`StatefulWriter.cpp`](CALM/fastdds-v2.6.11/overlay/src/cpp/rtps/writer/StatefulWriter.cpp) | pacing timer, oldest repair 우선 release, 남는 B로 새 전송 scheduling |
| [`RTPSWriter.cpp`](CALM/fastdds-v2.6.11/overlay/src/cpp/rtps/writer/RTPSWriter.cpp) | Writer 전송 경로 계측 및 연결 |

### Cyclone DDS 0.10.5: CALM 3 prototype

Cyclone 포팅은 per-proxy-reader 관측, retransmit enqueue 제어, oldest-first pacing과
held-new를 포함합니다. 표준 8개 일시 손실/혼잡 조건에서 Default와 CALM 모두
2,220/2,220 sample을 받았고, 평균 p95는 $\lambda<\mu$ 5조건에서 36.1%,
$\lambda\ge\mu$ 3조건에서 41.3% 감소했습니다. 다만 지속적인
`4 MiB x 10 Hz + PER 20%`에서는 과감쇠로 Default보다 악화했습니다. Fast DDS의
CALM 4.1 controller와 완전히 동일한 버전은 아닙니다.

## Repository Layout

```text
CALM/
  fastdds-v2.6.11/       # CALM 4.1 patch and source overlay
  cyclonedds-0.10.5/     # CALM 3 prototype patch and source overlay
experiments/             # ROS 2 package, profiles and automation scripts
docs/                    # current implementation and experiment reports
S1/
  calm_pretest/          # initial baseline snapshot
  document/              # S1-era proposal and legacy research documents
```

`CALM/`은 독립적으로 빌드하는 새 DDS 라이브러리가 아닙니다. `CALM.patch`를 정확한
upstream DDS 버전에 적용하고 해당 middleware와 ROS 2 RMW를 다시 빌드해야 합니다.
`overlay/`는 수정된 파일의 정확한 snapshot으로, 코드 검토와 복구를 위해
제공합니다. upstream middleware의 라이선스는 파생 소스에도 그대로 적용됩니다.

대용량 raw CSV와 ROS 2 `build/install/log` 산출물은 저장소에 포함하지 않습니다.

## Build and Use

### Fast DDS 2.6.11

```bash
git clone --branch v2.6.11 https://github.com/eProsima/Fast-DDS.git Fast-DDS
cd Fast-DDS
git apply /path/to/CALM/CALM/fastdds-v2.6.11/CALM.patch
```

### Cyclone DDS 0.10.5

```bash
git clone --branch 0.10.5 https://github.com/eclipse-cyclonedds/cyclonedds.git CycloneDDS
cd CycloneDDS
git apply /path/to/CALM/CALM/cyclonedds-0.10.5/CALM.patch
```

그 다음 ROS 2 Humble workspace에서 선택한 middleware와 RMW package를
`colcon build --symlink-install`로 빌드합니다. 실험 package는 `experiments/`를
workspace의 `src/calm_pretest_yw`로 배치해 함께 빌드할 수 있습니다.

<!-- ### Loopback example

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

자동화는 종료 시 loopback qdisc 제거를 시도합니다. 강제 중단했다면 다음으로
직접 확인할 수 있습니다.

```bash
sudo tc qdisc del dev lo root 2>/dev/null || true
tc qdisc show dev lo
```-->

<!-- ## CSV Metrics

`summary.csv`에서 우선 확인할 항목은 다음과 같습니다.

- `received_count`, `timed_out`, `elapsed_s`
- `sub_delay_mean_ms`, `sub_delay_p95_ms`, `sub_delay_std_ms`
- `sub_actual_hz`, `pub_actual_hz`
- `rho_max_bytes`, `rho_final_bytes` (`rho`는 현재 정의의 $U$와 같은 관측값)
- `sample_retransmit_count_p95/max`
- `sample_failed_repair_count_p95/max`
- `calm_active_rows`, `calm_budget_min/max_bytes`

Event-level `fastdds_storm_*.csv`에는 sample sequence, repair 상태, ACK progress,
failed repair, $\Delta U$, budget과 pacing period가 기록됩니다. -->

## Limitations and Next Work

- DDS-local `T_p` 일반식과 cold-start service-rate estimator 완성
- 실제 Wi-Fi의 여러 AP, RSSI, 간섭 조건에서 반복 검증
- 다중 Writer/Reader의 fairness와 Reader isolation 검증
- 지속적인 $\lambda>\mu$에서 source admission 또는 overload policy 검토
- Cyclone DDS controller를 CALM 4.1 식으로 통합하고 동일 matrix 재검증

CALM은 현재 연구용 prototype입니다. 실시간 또는 안전 필수 시스템에 적용하기
전에 workload별 검증이 필요합니다.

## Target Conference

- ACM/IEEE International Conference on Cyber-Physical Systems (ICCPS)
