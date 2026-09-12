# CALM 3.0 fragment feedback progress 개선 및 검증

## 1. 문제

검증 조건은 OPT 1, 2가 적용된 4 MiB x 10 Hz, 180 Mbps loopback,
3 ms +/- 1 ms 지연, 지속 20% packet loss, 100 samples, timeout 180 s이다.
생성률은 약 335.5 Mbps이므로 링크 모사 용량보다 크다.

기존 CycloneDDS CALM은 실제 재전송 fragment 중 일부가 전달되고 있어도
후속 NACKFRAG에 하나라도 다시 포함되면 `F`를 증가시켰다. 따라서
지속 손실에서 `B`와 `v`가 반복적으로 감소하여 Default보다 나빠졌다.

## 2. 구현 변경

### 2.1 실제 전송 fragment 추적

- repair generation마다 `sent_in_generation` bitmap을 초기화한다.
- 일반 NACKFRAG 재전송 경로와 CALM pacing 경로 모두 실제 enqueue에
  성공한 fragment index를 bitmap에 기록한다.
- feedback bitmap과 실제 전송 bitmap의 교집합만 비교한다.

### 2.2 feedback byte progress

한 feedback scope에서 다음 값을 계산한다.

```text
Q_sent     = feedback scope 안에서 실제 재전송한 byte
Q_repeat   = Q_sent 중 후속 NACKFRAG에서 다시 요청된 byte
Q_delivered_est = Q_sent - Q_repeat
r_residual = Q_repeat / Q_sent
```

- `Q_repeat < Q_sent`: fragment 복구가 진행 중인 round이다. F를 증가시키지 않는다.
- `Q_repeat = Q_sent`: 실제 전송 범위가 전혀 감소하지 않은 stalled round이다.
  이 경우에만 F를 증가시키고 multiplicative decrease를 허용한다.
- CALM 활성 중 `Q_delivered_est > 0`이면 그 비율에 따라 제한적인 additive
  increase를 적용할 수 있다. 이는 sample cumulative ACK 이전의 보조 성공
  신호이며 cumulative ACK 자체를 대체하지 않는다.

### 2.3 detector 정합성

CycloneDDS에 있던 다음 예외를 제거했다.

```text
rho >= 1 * S_bar AND F >= 1
```

최종 진입 backlog 조건은 설계와 동일하게 다음을 사용한다.

```text
rho >= 2 * S_bar
AND (rho non-shrinking rounds >= 2 OR ACK stall >= threshold)
```

F는 활성 후 감쇠 강도와 stalled repair 판정에 사용하며, 한 sample의
fragment 복구만으로 CALM 진입 임계값을 우회하지 않는다.

### 2.4 feedback guard

CycloneDDS OPT 2 조건에서 반복 feedback 간격은 실측 약 20 ms였다.
공용 100 ms guard를 사용하면 매 feedback이 새 generation으로 넘어가 F가
항상 0이 됐다. 자동화에 CycloneDDS 전용 10 ms 기본값을 추가했다.
Fast DDS의 기존 100 ms 기본값은 유지한다.

## 3. CycloneDDS 결과

| 방식 | 수신 | sub delay p95 | sub delay mean | rho max | CALM 활성 |
|---|---:|---:|---:|---:|---:|
| OPT12 Default | 62/100 | 7.75 s | 5.75 s | 4.19 MB | 0 |
| 기존 absolute-F CALM | 22/100 | 29.61 s | 16.42 s | 4.19 MB | 40,612 rows |
| 기존 absolute-F CALM 반복 | 21/100 | 30.44 s | 16.56 s | 4.19 MB | 39,710 rows |
| 잔여율 비례 감쇠 V4 | 23/100 | 26.55 s | - | 4.19 MB | 활성 |
| full-stall gate V5 | 30/100 | 21.45 s | - | 4.19 MB | 활성 |
| full-stall + progress AI V6 | 32/100 | 26.23 s | - | 4.19 MB | 활성 |
| 최종 strict detector V7 | 63/100 | 7.57 s | 5.69 s | 4.19 MB | 0 |

최종 V7에서는 F가 최대 4까지 관측됐지만 `rho_max = 1 * S_bar`였으므로
CALM은 활성화되지 않았다. CycloneDDS가 이미 한 sample 수준으로 repair
부채를 제한하는 이 조건에서는 CALM이 개입하지 않는 것이 맞다.
기존 CALM 대비 수신량은 약 3배, p95는 약 75% 감소했고 Default와 동등하다.

이 결과는 이 극한 조건에서 CALM이 Default보다 더 좋아졌다는 뜻이 아니라,
잘못된 CALM 활성화로 발생하던 회귀를 제거했다는 뜻이다. 지속적으로
`lambda > mu`이고 손실도 종료되지 않으므로 180 s 안의 100/100은 제어기의
현실적인 목표가 아니다.

## 4. Fast DDS 적용 결과

변경 전 백업:

```text
/home/csi/ros2_ws/CALM/document/backup/FastDDS_before_feedback_byte_progress_20260816_2350
```

Fast DDS에는 동일한 full-stall 판정을 실험 플래그로 추가했다.

```text
FASTDDS_CALM_FEEDBACK_BYTE_PROGRESS=1
```

플래그 기본값은 OFF이므로 현재 검증된 Fast CALM 동작은 그대로 유지된다.

| 방식 | 수신 | sub delay p95 | sub delay mean | rho max |
|---|---:|---:|---:|---:|
| OPT12 Default | 2/100 | 171.60 s | 125.55 s | 222.97 MB |
| 기존 Fast CALM | 100/100 | 123.78 s | 107.58 s | 67.21 MB |
| 기존 Fast CALM 반복 | 100/100 | 144.78 s | 126.03 s | 77.97 MB |
| 최종 full-stall gate | 100/100 | 135.25 s | 118.39 s | 72.04 MB |

최종 full-stall gate는 100/100 신뢰성을 유지했고, 기존 Fast CALM 두 실행의
중간 범위에 있다. 따라서 Fast DDS에서는 명확한 추가 성능 향상이 확인되지
않았으며 기본 활성화하지 않는다. 다만 이전의 잔여율 연속 감쇠 실험 평균
p95 약 140.65 s보다는 과감쇠가 줄었다.

## 5. 결론

1. F는 NACK 횟수가 아니라 실제 전송 fragment가 후속 feedback에서 전혀
   줄지 않은 경우에만 증가해야 한다.
2. fragment progress는 cumulative ACK 이전에 사용할 수 있는 보조 신호다.
3. CycloneDDS에서는 자체 WHC throttling 때문에 rho가 한 sample에 머무는
   조건이 있으며, 이때 CALM을 강제로 켜면 오히려 성능이 나빠진다.
4. `rho >= 2 * S_bar` 진입 조건을 우회하지 않아야 한다.
5. Fast DDS에서는 기존 CALM이 이미 Default의 폭풍을 크게 억제하므로 새
   정책은 실험 옵션으로만 유지한다.

