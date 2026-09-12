# CALM 3.0 파라미터 일반화 및 F 게이트 검증

작성일: 2026-08-15

## 1. 실험 목적

이번 실험은 다음 질문을 검증했다.

1. `F >= 1`을 CALM 활성화 조건의 AND 게이트로 추가해야 하는가?
2. 고정 byte로 설정한 `B_min`, `B_initial`, `B_max`, `AI_B`를 ReaderProxy가 관측한 평균 serialized sample 크기 `S_bar`에 비례하도록 일반화할 수 있는가?
3. 고정 release rate를 Writer가 관측한 offered rate 또는 ACK goodput으로 일반화할 수 있는가?
4. 서로 다른 payload, publish rate, CASE A/D/L 및 `lambda < mu`, `lambda >= mu` 조건에서 어떤 설정이 가장 안정적인가?

## 2. 실험 환경과 판정 기준

- DDS: Fast DDS 2.6.11 + OPT 1/2 + CALM
- 전송 경로: loopback
- 모사 링크: 180 Mbps, delay 3 ms, jitter 1 ms, baseline loss 0%
- 혼잡: CASE A, CASE D, CASE L의 일시 손실 또는 부하
- `lambda` 분류: main topic의 `payload x publish rate`
- 주요 지표: 수신 완주율, subscriber delay p95/평균/표준편차, 실제 수신 Hz, 최대 `rho`
- 최종 선택 원칙: CALM의 주 대상인 `lambda < mu` 성능을 우선하되, `lambda >= mu`에서 timeout이나 발산을 만들지 않아야 한다.

주의: 이 결과는 실제 Wi-Fi 최종 검증이 아니라 파라미터 후보를 빠르게 거르는 loopback 기반 검증이다.

## 3. 관측값 정의

### 3.1 평균 sample 크기 `S_bar`

`S_bar`는 사용자가 payload 크기를 입력해서 정하는 값이 아니다. Writer의 ReaderProxy가 현재 관리하는 Change들의 `serializedPayload.length`를 합산한 뒤 Change 수로 나눈 DDS 내부 관측값이다.

```text
S_bar = sum(serializedPayload.length_i) / number_of_changes
```

따라서 CDR/RTPS 처리 전후의 실제 ReaderProxy sample 크기를 사용하며, topic payload가 바뀌어도 같은 코드로 계산된다.

### 3.2 복구 부채 `rho`

`rho`는 Reader가 ACKNACK/NACKFRAG로 한 번 이상 복구를 요청했고, Writer가 아직 cumulative ACK으로 해소를 확인하지 못한 sample/fragment byte의 합이다.

### 3.3 실패 피드백 `F_i`

`F_i`는 sample `i`를 실제 재전송한 뒤, 최소 feedback guard가 지난 후 그 재전송 범위와 겹치는 NACK을 다시 받은 횟수다. 단순히 NACKFRAG 메시지가 여러 번 도착했다고 증가하지 않는다.

## 4. F를 CALM 진입 AND 게이트로 넣는 실험

비교식은 다음과 같다.

```text
현행:
rho >= 2*S_bar
AND (rho 비감소 2 round 이상 OR ACK 진행 정체 500 ms 이상)

F 게이트:
rho >= 2*S_bar
AND (rho 비감소 2 round 이상 OR ACK 진행 정체 500 ms 이상)
AND F_oldest >= 1
```

실험 조건은 1 MiB, 10 Hz, CASE A 20%, 240 samples이며 각 후보를 2회 반복했다.

| 후보 | 수신 | p95 중앙값 | 평균 delay 중앙값 | delay 표준편차 중앙값 | 실제 수신 Hz 중앙값 | 최대 rho 중앙값 |
|---|---:|---:|---:|---:|---:|---:|
| 현행 `rho_progress` | 480/480 | 2.329 s | 1.172 s | 0.608 s | 9.742 Hz | 9.68 MiB |
| `AND F>=1` | 480/480 | 6.032 s | 4.525 s | 1.085 s | 9.003 Hz | 21.73 MiB |

### 결론

`F`를 활성화 AND 조건에 넣지 않는다.

- `F>=1`이 되려면 실제 재전송, feedback guard, 반복 NACK이 먼저 필요하므로 예방적 pacing 시작이 늦어진다.
- 대기 중 `rho`와 held-new가 더 쌓여 delay와 jitter가 증가했다.
- `F=1`처럼 equality를 쓰면 `F=2`가 되는 순간 조건이 거짓이 되므로 의미상으로도 부적절하다. 게이트가 필요하다면 최소한 `F>=1`이어야 한다.
- 현행은 `F=0`에서도 oldest-first, shared budget, held-new, pacing을 시작하고, 확인된 실패가 생긴 뒤에만 `F`를 감쇠 강도 선택에 사용한다.

즉, `rho/progress`는 조기 스케줄링 신호이고 `F`는 강한 multiplicative decrease를 정당화하는 확인 신호다.

## 5. Budget 일반화 실험

요청한 `S_bar` 계수 후보를 축별로 비교했다.

- `B_min`: `1, 1/2, 1/4, 1/8, 1/16 * S_bar`
- `B_initial`: `3, 2, 1, 1/2, 1/4, 1/8, 1/16 * S_bar`
- `B_max`: `4, 3, 2, 1, 1/2 * S_bar`
- `AI_B`: `3, 2, 1, 1/2, 1/4, 1/8, 1/16 * S_bar`

`B_min <= B_initial <= B_max`를 위반하는 조합은 제어기의 물리적 제약상 제외했다. 단일 조건의 축별 최저값만 고르지 않고, 후보 bundle을 여러 workload에서 재검증했다.

### 5.1 주요 발견

- `B_initial=1/8*S_bar`, `1/16*S_bar`는 primary 조건에서 p95가 각각 약 11.65 s, 10.62 s로 지나치게 느렸다.
- `AI_B=1/4*S_bar`가 primary 축 실험에서 p95 1.791 s로 가장 좋았다.
- `B_initial=3*S_bar`는 한 조건에서는 빨랐지만 반복 실험에서 worst p95 10.640 s로 변동이 컸다.
- `B_initial=1*S_bar` bundle은 반복 민감 조건에서 가장 안정적이었다.

반복 검증 결과:

| Budget 비율 `(min, initial, max, AI)` | 512 KiB/20 Hz/A20 p95 중앙값 | 1 MiB/10 Hz/D p95 중앙값 | 512 KiB/30 Hz/A10 p95 중앙값 | 세 조건 worst |
|---|---:|---:|---:|---:|
| `(1/8, 1/2, 2, 1/4)*S_bar` | 3.720 s | 5.680 s | 6.245 s | 14.525 s |
| `(1/4, 1, 2, 1/4)*S_bar` | 2.546 s | 5.369 s | 5.082 s | 7.341 s |
| `(1/4, 3, 3, 1/4)*S_bar` | 1.547 s | 6.989 s | 5.661 s | 10.640 s |

순수 비례식 중 가장 안정적인 조합은 다음이었다.

```text
B_min     = 0.25*S_bar
B_initial = 1.00*S_bar
B_max     = 2.00*S_bar
AI_B      = 0.25*S_bar
```

그러나 순수 비례식은 작은 payload에서 기존 고정값보다 window가 작아져 `lambda < mu` 성능이 25.6% 나빠졌다. 따라서 고정 하한과 비례식을 합친 floor-anchored 하이브리드를 최종 선택했다.

```text
B_min     = max(128 KiB, 0.25*S_bar)
B_initial = max(512 KiB, 1.00*S_bar)
B_max     = max(  2 MiB, 2.00*S_bar)
AI_B      = max(256 KiB, 0.25*S_bar)
```

## 6. Release rate 일반화 실험

현재 고정 release rate는 다음과 같다.

```text
v_min     = 64 Mbps
v_initial = 192 Mbps
v_max     = 192 Mbps
AI_v      = +16 Mbps per cumulative ACK increase event
```

Writer가 새 Change의 byte와 생성 간격으로 offered-rate EWMA를 계산하는 기능을 추가하고, 다음 후보를 비교했다.

- offered low: `(0.5, 1.0, 2.0, 0.0625)*lambda_hat`
- offered balanced: `(0.5, 1.5, 2.5, 0.125)*lambda_hat`
- offered fast: `(0.75, 2.0, 3.0, 0.125)*lambda_hat`
- offered aggressive: `(1.0, 2.0, 4.0, 0.25)*lambda_hat`
- ACK goodput 2x
- 기존 fixed 64/192/192 Mbps, `AI_v=16 Mbps`

8개 workload에 모두 존재하는 후보의 p95 기하평균은 다음과 같다.

| release rate 후보 | 전체 p95 | `lambda < mu` p95 | `lambda >= mu` p95 | 완주 |
|---|---:|---:|---:|---:|
| fixed 64/192 Mbps | 6.129 s | 2.934 s | 20.916 s | 8/8 |
| offered balanced | 6.380 s | 3.289 s | 19.251 s | 8/8 |
| offered fast | 6.704 s | 3.648 s | 18.481 s | 8/8 |

offered-rate 비례형은 지속 과부하에서 일부 개선됐지만 CALM의 주 대상인 `lambda < mu`에서 일관되게 느렸다. `lambda_hat`만으로는 실제 서비스 용량 `mu`나 가용 여유 `mu-lambda`를 알 수 없기 때문이다. ACK goodput 기반 후보도 이번 검증에서는 고정형보다 좋지 않았다.

### 결론

- 기본 release rate는 fixed 64/192/192 Mbps와 `AI_v=16 Mbps`를 유지한다.
- offered-rate EWMA와 상대계수 기능은 후속 연구를 위해 코드에 남기되 기본값은 0으로 비활성화한다.
- `+16 Mbps`는 수학적으로 유일한 값이 아니라 이전 `2, 4, 8, 16 Mbps` 후보 실험에서 선택된 경험값이다.
- 진정한 링크 독립형 rate 일반화에는 offered rate 단독이 아니라 ACK service-rate, feedback RTT, outstanding debt를 함께 사용하는 보수적 capacity estimator가 필요하다.

## 7. 최종 하이브리드 성능

기존 고정 Budget과 최종 floor-anchored Budget을 동일한 8개 workload에서 비교했다.

### 7.1 `lambda < mu`

| workload | 기존 p95 | 하이브리드 p95 | 수신 |
|---|---:|---:|---:|
| 256 KiB, 20 Hz, A10 | 1.264 s | 1.039 s | 300/300 |
| 512 KiB, 20 Hz, A20 | 1.770 s | 1.653 s | 300/300 |
| 1 MiB, 10 Hz, D5 | 6.257 s | 5.484 s | 300/300 |
| 512 KiB, 10 Hz, L2 | 0.965 s | 1.012 s | 300/300 |
| 512 KiB, 30 Hz, A10 | 5.149 s | 5.379 s | 300/300 |

- p95 기하평균: 2.336 s -> 2.198 s, 약 5.9% 감소
- 5개 workload 모두 완주
- 두 조건에서는 각각 약 4.9%, 4.5% 악화됐으므로 모든 개별 조건에서 우월하다고 말할 수는 없다.

### 7.2 `lambda >= mu`

| workload | 기존 p95/수신 | 하이브리드 p95/수신 |
|---|---:|---:|
| 1 MiB, 25 Hz, A10 | 12.825 s, 240/240 | 12.268 s, 240/240 |
| 1 MiB, 30 Hz, A10 | 14.070 s, 240/240 | 14.035 s, 240/240 |
| 2 MiB, 15 Hz, D5 | 104.369 s, 197/240 timeout | 43.426 s, 240/240 |

- p95 기하평균: 26.606 s -> 19.554 s, 약 26.5% 감소
- 기존의 timeout 1건을 240/240 완주로 전환

## 8. 최종 반영값

```text
CALM activation:
  rho >= 2*S_bar
  AND (rho non-shrinking >= 2 feedback observations
       OR cumulative ACK progress stall >= 500 ms)

F usage:
  activation AND gate로 사용하지 않음
  F <= 1: B *= 0.80, v *= 0.90
  F >= 2: B *= 0.75, v *= 0.875

Budget:
  B_min     = max(128 KiB, 0.25*S_bar)
  B_initial = max(512 KiB, 1.00*S_bar)
  B_max     = max(2 MiB,   2.00*S_bar)
  AI_B      = max(256 KiB, 0.25*S_bar)

Release rate:
  v_min=64 Mbps, v_initial=192 Mbps, v_max=192 Mbps
  AI_v=16 Mbps

Pacing:
  floor=2 ms
  drain guard=1.10
```

## 9. 코드 반영 위치

- `ReaderProxy.h`: 하이브리드 Budget 및 release-rate 기본 상태값
- `ReaderProxy.cpp`: `S_bar`, offered-rate EWMA 관측, 상대계수 계산, budget/rate refresh, CSV 열 추가
- `automation.py`: 실제 자동화 기본값을 floor-anchored 하이브리드로 변경
- `ddsopt_loopback_automation.py`: 동일 기본값과 실험 인자, CALM CSV 요약 확장
- `calm_generalization_*.py`: F, Budget, rate 및 최종 하이브리드 재현 실험

## 10. 결과 폴더

- 전체 축 탐색 및 F 게이트: `CALM3_generalization_tuning_20260814_225942`
- Budget 교차 검증: `CALM3_generalization_budget_confirm_20260814_233622`
- Budget 반복 검증: `CALM3_generalization_budget_repeat_20260814_235332`
- rate 교차 검증: `CALM3_generalization_rate_confirm_20260815_000203`
- rate 추가 검증: `CALM3_generalization_rate_final_20260815_003525`
- 최종 하이브리드 검증: `CALM3_generalization_hybrid_20260815_004317`
- 최종 기본값 스모크: `DDSOPT_loopback_final_default_smoke_20260815_005149`

## 11. 해석 범위와 다음 검증

1. 현재 최종값은 loopback에서 가장 강건했던 후보이지, 모든 Wi-Fi 링크의 전역 최적값은 아니다.
2. 반복 횟수가 제한된 조건이 있으므로 실제 실험용 AP에서 동일한 A/B 반복 검증이 필요하다.
3. `lambda >= mu`는 제어로 지연을 줄일 수는 있어도 지속적인 offered-load 초과 자체를 제거할 수 없다.
4. 실제 Wi-Fi에서는 양방향 feedback 손실, 경쟁 단말, airtime contention이 추가되므로 완주율, p95/p99, jitter, 최대 rho, F 분포를 함께 확인해야 한다.
5. 다음 단계에서는 ACK service-rate estimator를 별도 후보로 설계한 뒤, fixed rate보다 반복적으로 우수할 때만 기본값으로 채택한다.
