# CALM 3.0 타이밍 파라미터 개선 및 검증 결과 (2026-08-14)

## 1. 목적

기존 Budget/감쇠/rate 최적 후보를 고정하고 다음 세 항목을 검증했다.

1. Pacing 주기
   - `T_P = max(T_P,min, 8B/v, g_drain * T_drain)`
   - `T_P,min = 1, 2, 4, 8 ms`
   - `g_drain = 1.05, 1.10, 1.25`
2. ACK progress stall
   - 기존 고정 500 ms
   - `T_stall = clamp(max(c1 * RTT_EWMA, c2 * T_HB), T_min, T_max)`
3. 반복 repair feedback guard
   - 기존 고정 100 ms
   - `T_guard = clamp(max(c3 * RTT_EWMA, T_HB), T_min, T_max)`

수정 전 구현은 다음 경로에 백업했다.

`CALM/document/backup/CALM3_tuned_20260814_before_timing`

## 2. 파라미터 선택 기준

후보는 다음 우선순위로 평가했다.

1. 요청한 sample을 모두 수신하고 timeout이 없어야 한다.
2. 수신 조건을 통과한 후보 중 p95 delay를 먼저 최소화한다.
3. max delay, delay 표준편차, inter-arrival jitter로 tail 안정성을 확인한다.
4. mean delay와 전체 완료시간을 보조 지표로 사용한다.
5. 단일 run은 netem 난수의 영향을 받으므로 최종 후보는 3회 중앙값으로 비교한다.
6. 대표 조건 외 CASE A/D/L과 서로 다른 Hz에서도 방향이 유지되는지 확인한다.

## 3. 이전 단계에서 채택한 제어 파라미터

공통 탐색 조건은 OPT1+2, 1024 KiB, 10 Hz, CASE A 20%, 600 samples였다.

| 항목 | 후보별 p95 delay | 채택값 | 선택 근거 |
|---|---|---:|---|
| `B_min` | 64K 10.31 s, 128K 10.06 s, 256K 13.11 s, 512K 8.10 s | 128 KiB | 단일 run 최저는 512K였지만 emergency clamp가 너무 커 반복 실패 때 감쇠 여지가 없었다. 128K를 포함한 최종 조합이 기존 조합보다 반복 p95와 max가 낮았다. |
| `B_initial` | 256K 13.05 s, 512K 10.03 s, 1024K 10.15 s | 512 KiB | 256K는 초기 drain이 느렸고, 1024K는 초기 burst가 커졌다. 512K가 가장 낮은 p95였다. |
| `B_max` | 512K 19.39 s, 1024K 11.91 s, 2048K 3.89 s | 2048 KiB | ACK 진행 후 회복 가능한 상한을 키워 tail backlog drain을 가장 빠르게 했다. |
| `AI_B` | 32K 25.89 s, 64K 37.65 s, 128K 18.81 s, 256K 5.83 s, 384K 12.67 s, 512K 12.59 s | 256 KiB | 작은 증가는 회복이 느리고 큰 증가는 재손실을 만들었다. |
| Soft `(gamma_B,gamma_v)` | (0.90,0.95) 4.30 s, (0.875,0.9375) 8.95 s, (0.80,0.90) 3.97 s | (0.80,0.90) | 첫 확인 실패에서 burst와 평균 rate를 충분히 줄이는 후보가 가장 빨랐다. |
| Hard `(gamma_B,gamma_v)` | (0.75,0.875) 3.21 s, (0.625,0.80) 27.42 s, (0.50,0.75) 34.77 s | (0.75,0.875) | 반복 실패에서 지나친 감쇠는 repair drain을 크게 늦췄다. |
| `AI_v` | 2M 51.58 s, 4M 48.91 s, 8M 38.05 s, 16M 6.05 s | 16 Mbps | ACK 진행 후 rate 회복이 가장 빨랐다. |
| ACK 증가 구조 | event 7.73 s, byte-normalized 8.11 s | event | 현재 부하에서 event 방식이 근소하게 빨랐다. |
| rate 구조 | fixed 7.73 s, ACK-goodput headroom 1.05/1.25/1.50/2.0 = 33.42/25.95/20.71/7.72 s | fixed | ACK-goodput은 현재 pacing에 제한된 goodput을 다시 목표로 삼는 자기제한이 생겼다. |

최종 조합 3회 중앙값은 OPT12 Default p95 72.88 s, 변경 전 CALM 6.40 s, 최종 CALM 5.35 s였다. 모든 후보를 독립적으로 최저값만 조합한 것이 아니라, 조합 후 반복 검증을 통과한 값을 채택했다.

## 4. Writer 관측 feedback RTT

Fast DDS 내부에 다음 Writer-only 측정을 추가했다.

```text
repair batch의 첫 실제 전송 시각
    -> 해당 repair byte의 cumulative ACK 또는 겹치는 repeated NACK 시각
    -> feedback RTT sample
    -> RTT_EWMA = 0.8 * previous + 0.2 * sample
```

이 값은 순수 네트워크 전파 RTT가 아니다. DDS Reader 응답과 ACKNACK/NACKFRAG 처리 지연을 포함한 제어 루프 feedback 지연이다. 외부 링크 용량이나 NIC 측정값은 사용하지 않는다.

1024 KiB, 10 Hz, OPT1+2 조건에서 다음 값이 관측됐다.

- `T_HB = 50 ms`
- `RTT_EWMA = 약 53~143 ms`
- 마지막 관측값 약 70 ms

새 `calm_budget.csv` 열:

- `feedback_rtt_ewma_ms`
- `ack_stall_threshold_ms`
- `feedback_guard_ms`
- `heartbeat_period_ms`

## 5. Pacing floor 결과

### 5.1 300-sample 탐색

3회 p95 중앙값:

| `T_P,min` | p95 | mean | max | delay std | jitter | 수신 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 ms | 3.66 s | 2.66 s | 3.93 s | 0.96 s | 170 ms | 900/900 |
| 2 ms | 5.42 s | 4.12 s | 5.96 s | 1.60 s | 166 ms | 900/900 |
| 4 ms | 4.14 s | 3.04 s | 4.38 s | 1.04 s | 166 ms | 900/900 |
| 8 ms | 4.93 s | 3.56 s | 5.40 s | 1.36 s | 165 ms | 900/900 |

짧은 탐색에서는 1 ms가 가장 좋아 보였다.

### 5.2 600-sample 최종 검증

3회 p95 중앙값:

| `T_P,min` | p95 | mean | max | delay std | jitter | 수신 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 ms | 7.51 s | 3.61 s | 8.19 s | 2.09 s | 168 ms | 1800/1800 |
| **2 ms** | **5.35 s** | 4.11 s | **6.28 s** | **1.28 s** | 166 ms | 1800/1800 |
| 4 ms | 8.22 s | 4.83 s | 9.19 s | 2.49 s | 173 ms | 1800/1800 |
| 8 ms | 6.03 s | 3.72 s | 6.62 s | 1.61 s | **164 ms** | 1800/1800 |

최종 선택은 2 ms다. 1 ms는 짧은 탐색에서는 빨랐지만 더 긴 repair backlog에서 tail과 변동성이 커졌다. 4/8 ms는 작은 batch가 drain된 뒤에도 다음 tick을 더 오래 기다리는 비용이 커졌다.

## 6. Drain guard 결과

최종 floor 2 ms, 600 samples, 3회 중앙값:

| `g_drain` | p95 | mean | max | delay std | jitter | elapsed | 수신 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.05 | 5.35 s | 4.11 s | 6.28 s | **1.28 s** | 166 ms | 66.4 s | 1800/1800 |
| **1.10** | **4.63 s** | **3.25 s** | **4.90 s** | 1.31 s | **165 ms** | **65.4 s** | 1800/1800 |
| 1.25 | 7.13 s | 3.89 s | 7.59 s | 2.25 s | 166 ms | 67.2 s | 1800/1800 |

1.10은 1.05 대비 p95 13.3%, mean 21.0%, max 22.0%를 줄였다. delay 표준편차는 약 2.2% 증가했지만 나머지 tail/완료 지표가 좋아 1.10을 채택했다. 1.25는 과도하게 보수적이었다.

## 7. ACK stall 고정/동적 비교

동적식:

```text
T_stall = clamp(max(c1 * RTT_EWMA, c2 * T_HB), 50 ms, 2000 ms)
```

단일 300-sample screening:

| 설정 | 실제 threshold 범위 | p95 |
|---|---:|---:|
| fixed 500 ms | 500 ms | 2.23 s |
| c1=2, c2=2 | 105~271 ms | 2.96 s |
| c1=4, c2=2 | 248~541 ms | 2.91 s |
| c1=8, c2=2 | 537~1045 ms | 3.45 s |
| c1=4, c2=1 | RTT 항 지배 | 7.12 s |
| c1=4, c2=4 | 최소 HB 항 200 ms | 2.81 s |

ACK stall과 `rho 비감소 2 feedback rounds`는 OR 조건이다. 여러 run에서 rho 비감소 조건이 먼저 CALM을 활성화하므로 동적 stall의 영향이 제한적이었고, 고정 500 ms보다 반복 가능한 우위를 보이지 못했다. 기본값은 fixed 500 ms를 유지한다.

## 8. Feedback guard 고정/동적 비교

고정 screening:

| guard | p95 |
|---:|---:|
| 25 ms | 28.02 s |
| 50 ms | 19.65 s |
| 100 ms | 2.23 s |
| 200 ms | 3.60 s |

25/50 ms는 아직 이전 batch의 피드백일 수 있는 NACK을 새 실패로 과계수해 B와 rate를 과도하게 감쇠했다.

동적식:

```text
T_guard = clamp(max(c3 * RTT_EWMA, T_HB), 10 ms, 2000 ms)
```

단일 screening p95는 c3=1/2/4에서 14.22/1.49/4.60 s였다. c3=2가 대표 A20 조건에서는 좋아 보였으나 다중 부하 3회 중앙값에서 일관되지 않았다.

| 조건 | fixed 100 ms | dynamic c3=2 | 변화 |
|---|---:|---:|---:|
| 512 KiB, 30 Hz, CASE A 10% | 6.04 s | 6.75 s | 11.7% 악화 |
| 1024 KiB, 10 Hz, CASE D 5 s | 5.68 s | 6.22 s | 9.4% 악화 |
| 512 KiB, 20 Hz, CASE L | 1.52 s | 2.67 s | 75.2% 악화 |

기본값은 fixed 100 ms를 유지한다. RTT 기반 동적 모드는 연구용 비교 옵션으로 남겼다.

## 9. 최종 채택값

```text
T_P,min             = 2 ms
drain_guard         = 1.10
ACK stall mode      = fixed
ACK stall           = 500 ms
feedback guard mode = fixed
feedback guard      = 100 ms
```

동적 모드는 다음 환경변수로 선택할 수 있다.

```text
FASTDDS_CALM_ACK_STALL_MODE=dynamic
FASTDDS_CALM_ACK_STALL_RTT_MULTIPLIER
FASTDDS_CALM_ACK_STALL_HB_MULTIPLIER
FASTDDS_CALM_ACK_STALL_MIN_MS
FASTDDS_CALM_ACK_STALL_MAX_MS

FASTDDS_CALM_FEEDBACK_GUARD_MODE=dynamic
FASTDDS_CALM_FEEDBACK_GUARD_RTT_MULTIPLIER
FASTDDS_CALM_FEEDBACK_GUARD_MIN_MS
FASTDDS_CALM_FEEDBACK_GUARD_MAX_MS
FASTDDS_CALM_FEEDBACK_RTT_ALPHA
```

## 10. 결과 위치와 한계

- 결과: `results/test_yw_2/CALM3_timing_20260814`
- 이전 파라미터 결과: `results/test_yw_2/CALM3_tuning_20260813`

모든 timing 후보는 loopback + netem에서 검증했다. DDS/UDP/qdisc/소켓/스케줄링 경로는 포함하지만 실제 Wi-Fi NIC retry, AP contention, 채널 간섭은 포함하지 않는다. 최종값은 실험용 Wi-Fi에서 동일 A/B 3회 검증이 필요하다.

## 11. 최종 기본값 smoke 검증

인자를 별도로 덮어쓰지 않은 `ddsopt_loopback_automation.py` 기본값으로 OPT1+2, 1024 KiB, 10 Hz, CASE A 20%, 300 samples A/B를 실행했다.

| 제어 | 수신 | p95 delay | timeout |
|---|---:|---:|---:|
| OPT1+2 Default | 300/300 | 56.65 s | 없음 |
| 최종 CALM | 300/300 | 7.61 s | 없음 |

최종 CALM은 완전 수신을 유지하면서 이 run의 p95를 약 86.6% 줄였다. 결과는 `DDSOPT_loopback_final_defaults_smoke_timing_20260814_141008`에 있다.

검증 상태:

- Fast DDS `fastrtps` 빌드 및 install 성공
- `calm_pretest_yw` 빌드 성공
- `automation.py`, `ddsopt_loopback_automation.py` Python 구문 검사 성공
- Fast DDS diff whitespace 검사 성공
