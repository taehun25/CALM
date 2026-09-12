# CALM 3.0 DDS 자율 Release Rate 설계 및 검증

## 1. 목적

기존 CALM의 `release_rate(v)`는 다음 고정 범위를 사용했다.

- `v_min = 64 Mbps`
- `v_initial = 192 Mbps`
- `v_max = 192 Mbps`
- `AI_v = 16 Mbps`

이 값은 180 Mbps loopback 모사 링크에서는 유효했지만, 링크가 달라지면 다시 튜닝해야 한다. 이번 변경의 목적은 NIC 통계, iperf, 운영체제 큐와 같은 하위 계층 정보를 사용하지 않고 Writer 측 DDS 이벤트만으로 `v`를 추정하고 조절하는 것이다.

모든 비교에서 OPT 1과 OPT 2는 동일하게 적용했다.

- OPT 1: `maxMessageSize = 1472 B`
- OPT 2: `heartbeatPeriod = publish period / 2`
- 대조군과 실험군의 차이는 CALM 및 release-rate 제어 방식뿐이다.

## 2. DDS 계층 관측값

Reader별로 다음 누적량과 시간만 관측한다.

- `O`: Writer에 새로 생성된 serialized byte와 생성 간격
- `A`: cumulative ACK base 전진으로 새로 ACK된 byte
- `A_repair`: ACK로 해소된 NACK-induced repair byte
- `X`: Writer가 DDS transport로 실제 release한 DATA/DATA_FRAG byte
- `rho`: 한 번 이상 NACK되었고 아직 ACK로 해소되지 않은 repair byte
- `F`: 실제 재전송한 영역이 이후 feedback에서 다시 요청된 횟수
- `RTT_EWMA`: 재전송 release부터 해당 영역의 재요청/ACK까지 관측된 DDS feedback 시간
- `T_HB`: Writer에 설정된 HEARTBEAT period

`X`는 DDS가 transport에 등록한 순간의 burst이므로 물리 링크 처리량으로 간주하지 않는다. 실제 서비스율 표본은 cumulative ACK 진행량을 사용한다.

## 3. 서비스율 추정

### 3.1 NORMAL

정상 상태에서는 Writer가 application-limited일 수 있다. 따라서 생성률과 ACK 진행률은 물리 링크 최대 용량이 아니라 현재 부하를 처리할 수 있다는 DDS-visible 하한이다.

\[
\hat{\mu}_{DDS} \leftarrow \max(\hat{\mu}_{DDS}, g_{offered}, g_{ack})
\]

### 3.2 CALM_ACTIVE

ACK 하나마다 갱신하면 ACK compression 때문에 순간 표본이 과대 추정된다. 다음 동적 관측 창이 끝났을 때만 서비스율을 갱신한다.

\[
T_{est}=\max(4T_{P,min},\ 2T_{HB},\ 4RTT_{EWMA})
\]

\[
g_{ack}=\frac{8A_{window}}{T_{window}}
\]

ACK compression 상한은 실제 DDS 명령률과 offered rate를 이용한다.

\[
g_{sample}=\min\left(g_{ack},\ 1.05\max(v_{command},g_{offered})\right)
\]

비대칭 EWMA는 다음과 같다.

\[
\hat{\mu}_{k+1}=\hat{\mu}_k+\alpha(g_{sample}-\hat{\mu}_k)
\]

- 상승 표본: `alpha_up = 0.25`
- 하락 표본: `alpha_down = 0.10`
- feedback 실패: `mu_hat <- 0.98 * mu_hat`

실패 시 `v`에는 기존 CALM의 multiplicative decrease도 적용된다. `mu_hat`은 장기 기준값이고 `v`는 즉시 제어값이므로 둘의 역할을 구분했다.

## 4. Release Rate 일반화

CALM 활성화 시점의 서비스율을 `mu_entry`라고 하면 다음과 같이 계산한다.

\[
v_{live}=\frac{8L_{atom}}{\max(T_{HB},T_{guard})}
\]

\[
v_{min}=\max(v_{live},0.25\hat{\mu}_{DDS})
\]

\[
v_{initial}=\max(v_{min},1.0\mu_{entry})
\]

\[
v_{max}=\max(v_{initial},2.0\max(\hat{\mu}_{DDS},\mu_{entry}))
\]

\[
AI_v=\max(v_{live},0.25\max(\hat{\mu}_{DDS},\mu_{entry}))
\]

`L_atom`은 현재 DDS DATA_FRAG의 최소 전송 단위다. 새 Change가 들어올 때 O(1)로 캐시하므로 제어 갱신마다 ReaderProxy 전체를 순회하지 않는다.

ACK와 repair 해소가 포함된 서비스 관측 창이 성공적으로 끝난 경우에만 한 번 probe한다. 모든 ACK마다 `AI_v`를 더하지 않는다.

## 5. Pacing

기존 CALM의 byte budget `B`와 동적 `v`로 pacing 주기를 계산한다.

\[
T_P=\max\left(T_{P,min},\frac{8B}{v},1.10T_{drain}\right)
\]

전송 순서는 기존 설계대로 oldest repair first다. 남은 repair는 다음 HEARTBEAT까지 기다리지 않고 pacing tick에서 계속 release한다. `rho=0`이면 NORMAL로 복귀하지만 서비스율 estimator는 계속 관측한다.

## 6. 실험 조건

- loopback `lo`
- `netem rate = 180 Mbps`
- `delay = 3 ms +/- 1 ms`
- baseline loss `0%`
- CASE A, CASE D, CASE L의 일시 손실/혼잡 사용
- 모든 실험에 OPT 1, 2 적용
- CALM 미적용 Optimized Default, 기존 고정 `v` CALM, DDS 동적 `v` CALM 비교

`lambda < mu` 검증 조건은 5개, `lambda >= mu` 검증 조건은 3개다. 동적 CALM의 최종 8개 조건은 모두 송수신을 완료했다.

## 7. 최종 결과

| 부하 구간 | 제어 | 수신 | p95 평균 | p95 중앙값 | jitter 중앙값 | 수신 Hz 중앙값 | 최대 rho |
|---|---|---:|---:|---:|---:|---:|---:|
| `lambda < mu` 5개 | 고정 v CALM | 1500/1500 | 3.015 s | 1.598 s | 0.465 s | 19.562 | 58.07 MiB |
| `lambda < mu` 5개 | 동적 v CALM | 1500/1500 | 3.455 s | 2.979 s | 0.874 s | 17.546 | 51.71 MiB |
| `lambda >= mu` 3개 | 고정 v CALM | 720/720 | 23.189 s | 13.949 s | 3.657 s | 10.245 | 70.73 MiB |
| `lambda >= mu` 3개 | 동적 v CALM | 720/720 | 20.052 s | 14.480 s | 3.703 s | 10.551 | 92.76 MiB |
| 전체 8개 | 고정 v CALM | 2220/2220 | 10.580 s | 5.417 s | 1.350 s | 10.480 | 70.73 MiB |
| 전체 8개 | 동적 v CALM | 2220/2220 | 9.679 s | 5.006 s | 1.302 s | 10.893 | 92.76 MiB |

전체 기준 동적 방식은 고정 방식보다 다음과 같이 변했다.

- p95 평균: 약 `8.5%` 감소
- p95 중앙값: 약 `7.6%` 감소
- jitter 중앙값: 약 `3.6%` 감소
- 수신 Hz 중앙값: 약 `3.9%` 증가
- 신뢰성: 두 방식 모두 `2220/2220`

가장 심한 `2048 KiB x 15 Hz, CASE D`에서는 다음 결과가 나왔다.

- Optimized Default: `1/240` 수신 후 timeout
- 고정 v CALM: `240/240`, p95 `42.212 s`
- 동적 v CALM: `240/240`, p95 `34.101 s`

동적 방식은 이 조건에서 고정 방식보다 p95를 약 `19.2%` 줄였다.

## 8. 냉정한 판단

DDS-only 동적 제어는 구현 및 loopback 검증을 완료했다. 외부 링크 용량을 입력하지 않고도 모든 검증 조건에서 완전 수신을 달성했고, 전체 및 과부하 조건의 평균 성능은 고정 `v`보다 좋아졌다.

그러나 모든 조건에서 우월하지는 않다. `lambda < mu`의 p95 평균은 고정 방식보다 약 `14.6%` 증가했고, `512 KiB x 10 Hz CASE L`처럼 고정 `v`가 더 빠른 조건도 있었다. 최대 `rho`도 전체 기준 고정 방식보다 커졌다. 따라서 현재 결과는 고정값 의존성을 제거한 일반화 구현의 성공이지, 모든 Wi-Fi에서 최적임을 입증한 결과는 아니다.

다음 검증은 실제 실험용 Wi-Fi에서 동일 조건 반복 A/B를 수행하고, 링크별 고정값 재튜닝 없이 동적 방식이 신뢰성과 tail delay를 유지하는지 확인해야 한다.

## 9. 결과 위치

- 최종 후보 탐색 및 고정/Default 비교: `/home/csi/ros2_ws/results/test_yw_2/CALM3_dds_service_rate_tuning_20260815_183456`
- 최종 safe-window 8조건 검증: `/home/csi/ros2_ws/results/test_yw_2/CALM3_dds_service_safe_validation_20260815_190003`
- 최종 기본 인자 smoke: `/home/csi/ros2_ws/results/test_yw_2/DDSOPT_loopback_dds_service_final_default_smoke_20260815_194110`
- 변경 전 백업: `/home/csi/ros2_ws/CALM/document/backup/CALM3_before_dds_service_rate_20260815_172945`
