# CALM 3.0 Cyclone DDS 포팅 및 loopback 검증

## 1. 결론

Cyclone DDS 0.10.5의 Reliable Writer 경로에 CALM 3.0을 구현했다. OPT 1, 2를
동일하게 적용한 Optimized Default와 비교한 8개 표준 일시 손실/혼잡 조건에서
Default와 CALM 모두 2,220/2,220 sample을 수신했다.

- lambda < mu 5조건 p95 평균: 185.1 ms -> 118.2 ms, 36.1% 감소
- lambda >= mu 3조건 p95 평균: 490.2 ms -> 287.9 ms, 41.3% 감소
- 모든 표준 조건에서 timeout 없음
- 실제 CALM이 활성화된 대표 조건의 추가 반복에서도 p95 감소 방향 유지
- 반면 jitter 표준편차와 수신 Hz는 일부 조건에서 소폭 악화
- 4 MiB x 10 Hz와 지속 20% loss를 동시에 준 비정상적 지속 과부하에서는
  과감쇠로 CALM이 Default보다 나빴음

따라서 현재 포팅은 목표인 **일시적 손실/혼잡에서 완전 수신을 유지하면서 tail
delay를 줄이는 것**에는 유효하지만, 지속적인 lambda > mu와 높은 독립 PER까지
안정적으로 해결하는 최종 알고리즘은 아니다.

## 2. 구현

### Reader별 관측 상태

Cyclone DDS의 Writer-ProxyReader match마다 다음 상태를 유지한다.

- rho: 한 번 이상 sample/fragment repair가 NACK된 뒤 cumulative ACK으로
  해소되지 않은 WHC sample byte
- pending repair byte와 fragment bitmap
- sample별 실제 재전송 횟수 N
- 전송한 repair 영역이 100 ms 이후 feedback에서 다시 요청된 횟수 F
- cumulative ACK high sequence와 마지막 ACK 진전 시각
- 평균 serialized sample 크기
- offered rate와 ACK 기반 DDS service-rate EWMA
- CALM의 B, v, T_P, held-new 목록

NACKFRAG의 rho는 누락 fragment 합이 아니라 해당 sample 전체를 한 번만 계상한다.
이는 Fast DDS에서 사용한 “NACK된 뒤 ACK되지 않은 WHC sample byte”와 관측 의미를
맞추기 위한 것이다. 실제 pacing 대상은 여전히 요청된 fragment만이다.

### CALM 진입

기본 진입식은 다음과 같다.

```text
rho >= 2*S_bar
AND (rho 비감소 feedback round >= 2 OR cumulative ACK stall >= 500 ms)
```

Cyclone DDS는 하나의 fragmented sample을 복구한 뒤 다음 sample로 전진하는
경향이 강해 rho가 2*S_bar까지 자라지 않는 경우가 많다. 따라서 실제 실패가
확인된 단일 repair frontier를 위한 다음 보조 조건을 추가했다.

```text
rho >= S_bar AND F >= 1
```

이 보조 조건도 rho 비감소 또는 ACK stall 조건과 함께 만족해야 한다. 정상적인
첫 NACK에는 개입하지 않는다.

### 제어

- B_min = max(128 KiB, 0.25*S_bar)
- B_initial = max(512 KiB, 1.0*S_bar)
- B_max = max(2 MiB, 2.0*S_bar)
- AI_B = max(256 KiB, 0.25*S_bar)
- F=1: B x 0.80, v x 0.90
- F>=2: B x 0.75, v x 0.875
- cumulative ACK 진전: B와 v를 additive increase
- v는 Writer가 관측한 offered rate와 cumulative ACK service rate로 추정
- T_P = max(2 ms, 8B/v, 1.10*T_drain)

각 pacing tick은 oldest pending repair부터 B 이내로 release한다. repair 요청을
모두 release한 뒤 B가 남으면 held-new를 release한다. write() 자체는 막지 않고
새 Change를 WHC에 넣되 네트워크 release만 보류한다. rho와 held-new가 모두
해소되면 NORMAL로 복귀한다.

### Cyclone DDS 삽입 위치

- `ddsi_entity_match.c`: Writer-Reader match별 CALM 상태 생성/해제
- `q_receive.c`: ACKNACK/NACKFRAG 수락 뒤 detector 실행, CALM_ACTIVE이면
  기본 즉시 retransmit enqueue를 보류
- `q_transmit.c`: CALM_ACTIVE 중 새 Change를 held-new로 전환
- `q_xevent.c`: queued repair byte 조회
- `ddsi_storm_observer.c`: rho/N/F/ACK 관측, 동적 B/v, oldest-first pacing,
  held-new release, CSV 계측

Cyclone DDS에는 Fast DDS와 동일한 ReaderProxy 상태 열거와 T_NR callback이 없다.
따라서 CALM은 ACKNACK/NACKFRAG가 수락된 직후, 기본 retransmit queue에 등록되기
직전에 실행된다. 이는 Cyclone에서 T_NR 종료에 대응하는 가장 가까운 제어점이다.

## 3. 실험 조건

- DDS: Cyclone DDS 0.10.5, `rmw_cyclonedds_cpp`
- 전송: UDP loopback
- netem: 180 Mbps, 3 ms +/- 1 ms, baseline loss 0%
- OPT 1: MaxMessageSize/MaxRexmitMessageSize 1472 B
- OPT 2: heartbeat period를 publish period의 1/2로 설정
- Default: OPT 1, 2 + CALM OFF
- CALM: OPT 1, 2 + CALM ON
- 양쪽 main QoS: Reliable KEEP_LAST, workload count 이상의 동일 depth
- CASE A loss와 CASE D/L 부하는 일시적으로만 적용

## 4. 표준 8조건 결과

| 부하 | 조건 | Default 수신 | CALM 수신 | Default p95 | CALM p95 | CALM 활성 |
|---|---|---:|---:|---:|---:|---:|
| lambda < mu | 256 KiB x 20 Hz, A10 | 300/300 | 300/300 | 163.2 ms | 179.9 ms | 예 |
| lambda < mu | 512 KiB x 20 Hz, A20 | 300/300 | 300/300 | 59.8 ms | 31.6 ms | 예 |
| lambda < mu | 1 MiB x 10 Hz, D5 | 300/300 | 300/300 | 57.3 ms | 57.0 ms | 아니오 |
| lambda < mu | 512 KiB x 10 Hz, L2 | 300/300 | 300/300 | 356.2 ms | 51.4 ms | 아니오 |
| lambda < mu | 512 KiB x 30 Hz, A10 | 300/300 | 300/300 | 288.7 ms | 271.2 ms | 예 |
| lambda >= mu | 1 MiB x 25 Hz, A10 | 240/240 | 240/240 | 659.2 ms | 111.5 ms | 예 |
| lambda >= mu | 1 MiB x 30 Hz, A10 | 240/240 | 240/240 | 480.6 ms | 270.9 ms | 예 |
| lambda >= mu | 2 MiB x 15 Hz, D5 | 240/240 | 240/240 | 330.8 ms | 481.4 ms | 아니오 |

CALM이 활성화되지 않은 CASE D/L의 차이는 CALM 효과가 아니라 실행별 네트워크
스케줄링 변동이다. 해당 실행은 Default와 사실상 같은 경로였다.

### 대표 조건 반복

512 KiB x 20 Hz, 일시 20% loss를 두 번 비교했다.

- Default p95 평균: 55.9 ms
- CALM p95 평균: 32.1 ms
- p95 42.5% 감소, 양쪽 모두 600/600 수신
- jitter 평균: Default 160.3 ms, CALM 176.2 ms

1 MiB x 30 Hz, 일시 10% loss를 두 번 비교했다.

- Default p95 평균: 405.9 ms
- CALM p95 평균: 274.9 ms
- p95 32.3% 감소, 양쪽 모두 480/480 수신
- jitter 평균: Default 144.3 ms, CALM 158.2 ms

즉 p95 tail delay 개선은 반복됐지만 jitter 개선은 확인되지 않았다.

## 5. 지속 과부하 반례

4 MiB x 10 Hz는 payload만으로 335.5 Mbps여서 180 Mbps 링크 용량을 계속
초과한다. 여기에 종료되지 않는 20% packet loss를 적용한 결과는 다음과 같다.

- Default: publish 63, receive 62/100, timeout, p95 7.75 s
- CALM: publish 23, receive 22/100, timeout, p95 29.61 s
- CALM의 v가 25.1 Mbps까지 감소
- F 최대값: Default 23, CALM 39

큰 sample은 수천 fragment로 구성되므로 독립 20% PER에서는 일부 fragment의
반복 실패가 필연적이다. 현재 제어는 이를 혼잡 신호로 해석해 B와 v를 계속
줄였고, 결과적으로 과감쇠됐다. 향후에는 F의 절대값만 사용하지 않고
“feedback 사이 requested-fragment byte가 감소하는가”를 함께 봐야 한다.

## 6. 검증 및 결과 위치

- Cyclone DDS 전체 단위 테스트: 1280/1280 통과, upstream disabled 1개
- 기본 빌드: `cyclonedds`, `rmw_cyclonedds_cpp` 통과
- 8조건 1차 결과:
  `CALM3_cyclonedds_dds_service_validation_20260816_222612`
- 중단 지점 이후 5조건:
  `CALM3_cyclonedds_dds_service_validation_20260816_223041`
- lambda < mu 대표 반복:
  `DDSOPT_loopback_cyclonedds_cyclone_calm_repeat_lt_20260816_224318`
- lambda >= mu 대표 반복:
  `DDSOPT_loopback_cyclonedds_cyclone_calm_repeat_ge_20260816_224410`
- 지속 과부하 반례:
  `DDSOPT_loopback_cyclonedds_cyclone_calm_stress_ge_20260816_223600`

## 7. 현재 판단

Cyclone DDS 포팅은 기능적으로 완료됐다. CALM OFF에서는 기존 Cyclone의 즉시
재전송 경로가 유지되고, CALM ON에서만 detector, budget, dynamic release rate,
oldest-first pacing, held-new가 동작한다.

표준 일시 손실/혼잡 범위에서는 완전 수신을 유지하면서 p95를 줄였으므로
Cyclone DDS에서도 CALM의 핵심 메커니즘이 작동한다고 판단한다. 다만 실제 Wi-Fi
반복 검증, 다중 Reader에서의 held-new 정책, jitter 개선, 지속 높은 PER에서의
F 과감쇠 방지는 남은 연구 과제다.
