# CALM 3.0 파라미터 개선 및 검증 결과 (2026-08-14)

## 1. 목적과 변경 전 백업

- 목적: OPT 1, 2를 적용한 Reliable Fast DDS에서 `lambda <= mu`일 때 일시적 손실/혼잡으로 발생하는 재전송 burst와 tail delay를 줄이면서 완전 수신을 유지한다.
- 변경 전 백업: `/home/csi/ros2_ws/CALM/document/backup/CALM3_rho_progress_fgain_20260813_224130`
- 백업에는 관련 소스 10개, Fast DDS working-tree patch, git 상태, SHA-256 목록이 포함되어 있다.
- 전체 실험 결과: `/home/csi/ros2_ws/results/test_yw_2/CALM3_tuning_20260813`

## 2. 공통 실험 조건

- Fast DDS 2.6.11, Reliable QoS
- OPT 1: `maxMessageSize=1472 B`
- OPT 2: publish 주기에 연동한 Heartbeat period
- loopback `lo`에 netem 적용
  - 링크 rate: 180 Mbps
  - delay: 3 ms
  - jitter: 1 ms
  - baseline loss: 0%
- 고용량 storm CSV는 탐색 속도와 디스크 사용량을 위해 비활성화했다.
  - 따라서 `summary.csv`의 `rho_max=0`은 실제 rho가 0이라는 뜻이 아니다.
  - CALM 실행의 실제 rho와 상태는 각 run의 `calm_backlog.csv`에 기록되어 있다.
- loopback은 DDS/UDP/qdisc/소켓/스케줄링 경로를 검증하지만 실제 Wi-Fi NIC, AP contention, 무선 재시도는 포함하지 않는다.

## 3. 최종 채택 파라미터

| 항목 | 변경 전 | 최종 채택 | 의미 |
|---|---:|---:|---|
| `B_min` | 512 KiB | 128 KiB | 반복 실패 시 허용할 최소 batch |
| `B_initial` | 1024 KiB | 512 KiB | CALM 진입 직후의 batch window |
| `B_max` | 1024 KiB | 2048 KiB | ACK 회복 후 허용할 최대 batch window |
| `AI_B` | 256 KiB | 256 KiB | ACK 진전 시 window 증가량 |
| Soft `gamma_B` | 0.875 | 0.80 | 첫 확인 실패의 B 감쇠 |
| Soft `gamma_v` | 0.9375 | 0.90 | 첫 확인 실패의 rate 감쇠 |
| Hard `gamma_B` | 0.75 | 0.75 | 반복 실패의 B 감쇠 |
| Hard `gamma_v` | 0.875 | 0.875 | 반복 실패의 rate 감쇠 |
| `AI_v` | 16 Mbps | 16 Mbps | ACK 진전 시 release rate 증가량 |
| ACK 증가 구조 | event | event | cumulative ACK 이벤트당 증가 |
| rate 구조 | fixed AI | fixed AI | ACK마다 `AI_v` 증가, 실패 시 감쇠 |

`B_initial`과 `B_max`는 분리하는 편이 좋았다. CALM 진입 직후에는 512 KiB로 burst를 제한하고, ACK가 진행되면 2 MiB까지 회복할 수 있어 tail delay를 줄였다.

## 4. `lambda <= mu` 일반화 검증

메인 토픽의 제시 부하는 180 Mbps보다 작다. CASE D/L/A의 disturbance만 일시적으로 적용했다.

| 조건 | 메인 제시 부하 | disturbance | Default p95 | 최종 CALM p95 | 완전 수신 |
|---|---:|---|---:|---:|---:|
| 256 KiB, 20 Hz | 41.94 Mbps | CASE A 10%, 5초 | 12.82 s | 1.27 s | 양쪽 1000/1000 |
| 512 KiB, 20 Hz | 83.89 Mbps | CASE A 20%, 5초 | 47.57 s | 1.41 s | 양쪽 1000/1000 |
| 1024 KiB, 10 Hz | 83.89 Mbps | CASE D 100%, 5초 | 123.51 s | 5.35 s | 양쪽 1000/1000 |
| 512 KiB, 10 Hz | 41.94 Mbps | CASE L, 512 KiB x 20 Hz x 2, 5초 | 16.22 s | 1.04 s | 양쪽 1000/1000 |
| 512 KiB, 10 Hz | 41.94 Mbps | CASE L, 1024 KiB x 20 Hz x 4, 5초 | 15.33 s | 1.85 s | 양쪽 1000/1000 |

결론:

- 다섯 조건 모두 완전 수신을 유지했다.
- 최종 CALM의 p95 감소율은 Default 대비 약 87.9~97.0%였다.
- `calm_backlog.csv`에서 CALM_ACTIVE와 실제 rho 증가를 확인했다.
- 예: CASE D에서 rho는 최대 60,820,888 B까지 증가한 뒤 최종 0으로 해소됐다.

## 5. `lambda > mu` 스트레스 검증

이 조건은 메인 입력 자체가 180 Mbps를 넘기 때문에 낮은 지연과 완전 신뢰성을 동시에 보장할 수 없다. CALM의 목표는 발산/timeout을 완결 가능한 backlog로 바꾸는 것이다.

| 조건 | 메인 제시 부하 | disturbance | Default | 최종 CALM |
|---|---:|---|---|---|
| 1024 KiB, 30 Hz | 251.66 Mbps | CASE A 10%, 5초 | 600/600, p95 158.77 s | 600/600, p95 31.38 s |
| 2048 KiB, 15 Hz | 251.66 Mbps | CASE D 100%, 5초 | 1/600, timeout | 600/600, p95 69.63 s |
| 2048 KiB, 20 Hz | 335.54 Mbps | CASE L, 1024 KiB x 20 Hz x 4, 5초 | 1/600, timeout | 600/600, p95 84.01 s |

결론:

- 최종 CALM은 세 조건 모두 600/600을 완료했다.
- 그러나 p95 31~84초이므로 실시간 통신 성공으로 해석하면 안 된다.
- 지속적인 `lambda > mu`의 근본 해법은 application admission/rate control이며 repair 제어만으로 물리적 용량 부족을 없앨 수 없다.

## 6. 반복 A/B 검증

조건: 1024 KiB, 10 Hz, CASE A 20%, 600 samples, 각 3회.

| 지표의 3회 중앙값 | OPT12 Default | 변경 전 CALM | 최종 CALM |
|---|---:|---:|---:|
| p95 delay | 72.88 s | 6.40 s | 5.35 s |
| mean delay | 56.51 s | 3.63 s | 4.11 s |
| max delay | 75.83 s | 7.25 s | 6.28 s |
| delay 표준편차 | 11.80 s | 1.73 s | 1.28 s |
| inter-arrival jitter | 2.15 s | 0.171 s | 0.170 s |
| 총 수신 | 1800/1800 | 1800/1800 | 1800/1800 |

최종 후보는 변경 전 CALM보다 p95와 max, 지연 분산이 좋아졌지만 mean delay는 약간 나빠졌다. 따라서 모든 지표에서 절대 우위인 것은 아니며, 이번 선택은 tail delay와 변동성 억제를 우선한 결과다.

## 7. 파라미터 탐색 핵심 결과

동일 조건의 단일 run은 netem 난수 변동을 포함하므로 최종 후보는 위 3회 반복으로 다시 검증했다.

### 7.1 Budget

- `B_min` p95: 64K 10.31 s, 128K 10.06 s, 256K 13.11 s, 512K 8.10 s
- `B_initial` p95: 256K 13.05 s, 512K 10.03 s, 1024K 10.15 s
- `B_max` p95: 512K 19.39 s, 1024K 11.91 s, 2048K 3.89 s
- `B_max=2 MiB`가 회복 후 backlog drain을 가장 빠르게 했다.

### 7.2 ACK 증가량

- `AI_B` p95: 32K 25.89 s, 64K 37.65 s, 128K 18.81 s, 256K 5.83 s, 384K 12.67 s, 512K 12.59 s
- 너무 작은 증가량은 회복이 늦고, 너무 큰 증가량은 재손실을 유발했다.
- 256 KiB를 유지했다.

### 7.3 감쇠율

- Soft p95: `(0.90,0.95)` 4.30 s, `(0.875,0.9375)` 8.95 s, `(0.80,0.90)` 3.97 s
- Hard p95: `(0.75,0.875)` 3.21 s, `(0.625,0.80)` 27.42 s, `(0.50,0.75)` 34.77 s
- 첫 실패에는 적당한 감쇠가 유효했지만 반복 실패를 지나치게 강하게 감쇠하면 drain이 크게 느려졌다.

### 7.4 Release rate 증가량

- `AI_v` p95: 2 Mbps 51.58 s, 4 Mbps 48.91 s, 8 Mbps 38.05 s, 16 Mbps 6.05 s
- 16 Mbps를 유지했다.

## 8. 대안 구조 평가

### 8.1 ACK byte 정규화 증가

새 실험 모드 `FASTDDS_CALM_ACK_INCREASE_MODE=byte_normalized`를 추가했다.

- 현행 event: cumulative ACK 이벤트마다 `B += AI_B`
- byte-normalized: ACK된 byte credit이 현재 congestion window 한 개만큼 누적될 때 `B += AI_B`
- 결과: event p95 7.73 s, byte-normalized p95 8.11 s

byte-normalized는 ACK 분할/병합에 덜 민감하다는 구조적 장점이 있지만 현재 조건에서 성능 우위가 없어 기본값은 `event`로 유지했다.

### 8.2 Writer ACK-goodput 기반 rate

Writer가 관측할 수 있는 값만 사용한다.

```text
sample_goodput = acknowledged_bytes * 8 / ACK_interval
g_ack = 0.8 * g_ack_prev + 0.2 * sample_goodput
v_target = clamp(v_min, headroom * g_ack, v_max)
```

새 실험 모드 `FASTDDS_CALM_RELEASE_RATE_MODE=ack_goodput`를 추가했다.

- headroom 1.05: p95 33.42 s
- headroom 1.25: p95 25.95 s
- headroom 1.50: p95 20.71 s
- headroom 2.00: p95 7.72 s
- fixed AI_v=16: p95 7.73 s

ACK-goodput은 링크 용량 사전 입력이 필요 없다는 장점이 있다. 하지만 측정 goodput 자체가 현재 pacing에 제한되므로 작은 headroom에서는 낮은 rate를 다시 목표로 삼는 자기제한이 발생했다. 2.0에서는 fixed 방식과 같았지만 더 좋지 않았으므로 기본값은 `fixed`로 유지했다.

## 9. 코드 변경

- `ReaderProxy.h`
  - 최종 기본 Budget/Soft/Hard/rate 파라미터 반영
  - ACK byte credit 및 ACK-goodput headroom 상태 추가
- `ReaderProxy.cpp`
  - event/byte-normalized ACK 증가 모드
  - fixed/ACK-goodput release-rate 모드
  - CALM 진입, 실패 감쇠, 복구 시 ACK credit 초기화
- `ddsopt_loopback_automation.py`
  - 두 실험 모드와 headroom CLI/env 연결
  - 최종 파라미터를 기본값으로 반영
  - CASE A 선택형 transient duration 추가 (`0`은 기존 지속 손실 유지)
- `automation.py`
  - 실제 무선 자동화에서도 최종 Budget/감쇠 기본값 사용

## 10. 검증 상태와 다음 단계

- Fast DDS `fastrtps` 빌드 성공
- `calm_pretest_yw` 빌드 성공
- 두 Python 자동화 `py_compile` 성공
- 기본값 smoke A/B: Default p95 83.85 s, CALM p95 8.89 s, 양쪽 600/600
- 현재 Fast DDS 빌드 설정에는 unit-test target이 생성되지 않아 ReaderProxy CTest는 실행하지 못했다.

다음 단계:

1. 실험용 Wi-Fi에서 동일한 OPT12 Default/CALM paired test를 반복한다.
2. 무선에서는 최소 3회 반복하고 중앙값과 IQR을 사용한다.
3. `lambda <= mu` 다섯 조건을 우선 검증한다.
4. ACK-goodput은 기본 제어가 아니라 관측/향후 hybrid rate estimator 후보로 유지한다.
5. `lambda > mu` 결과는 폭풍 완화/완결성 평가로만 사용하고 실시간성 개선 근거로 사용하지 않는다.
