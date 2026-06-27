# CALM: Cytokine-Inspired Suppression of Retransmission Storms in ROS 2 DDS

CALM(Concentration-Aware Loop Modulation)은 ROS 2 DDS의 `RELIABLE` QoS에서 발생하는 재전송 폭풍(retransmission storm)을 완전 신뢰성을 유지한 채 가라앉히기 위한 연구 프로젝트입니다.

무선 ROS 2 환경에서는 평균 송신률이 채널 용량 안에 있더라도, 손실 스파이크나 링크 단절-복구 순간에 DDS의 `Heartbeat -> AckNack -> Retransmission` 루프가 스스로 부하를 키울 수 있습니다. CALM은 이 루프에 writer-local 음성 피드백을 넣어, 재전송 backlog가 커지는 순간 재전송 volume만 조절합니다.

> 핵심 아이디어: 재전송 backlog의 증가 자체를 억제 신호로 사용한다. 외부 혼잡 신호나 수신측 변경 없이, writer가 reader별 backlog를 보고 재전송량을 스스로 낮춘다.

## Motivation

ROS 2는 DDS/RTPS를 통해 통신하며, `RELIABLE` QoS는 손실된 샘플을 `AckNack`과 재전송으로 복구합니다. 하지만 무선 환경에서는 이 신뢰성 루프가 다음과 같은 양성 피드백에 빠질 수 있습니다.

1. 일부 메시지가 손실된다.
2. Reader가 누락 구간을 `AckNack`으로 요청한다.
3. Writer가 대량 재전송을 수행한다.
4. 재전송 트래픽이 추가 부하가 되어 더 많은 손실을 만든다.
5. 더 많은 `AckNack`과 재전송이 발생한다.

이 문제는 두 상황에서 특히 커집니다.

- **Lossy link:** 손실 스파이크가 다수의 재전송 요청을 만들고, 재전송 burst가 순간적으로 채널을 포화시킨다.
- **Disconnect-reconnect:** 링크 단절 중 backlog가 쌓이고, 재연결 순간 큰 범위의 누락 요청이 한꺼번에 재전송 burst를 만든다.

기존 DDS의 정적 knob, 예를 들어 history depth, lifespan, heartbeat period, flow controller, 재전송 큐 상한은 backlog 증가에 반응하는 closed-loop 억제가 아닙니다. 일부는 샘플 폐기나 정적 rate 제한으로 이어질 수 있어 완전 신뢰성의 목적과도 충돌합니다.

## Biological Inspiration

CALM은 사이토카인 폭풍을 억제하는 면역계의 음성 피드백에서 영감을 얻습니다.

면역계에서는 신호 물질의 농도가 높아질수록, 그 신호를 억제하는 반응도 함께 강해집니다. 즉, 신호 자체가 브레이크를 부르는 구조입니다. CALM은 이 원리를 RTPS 재전송 루프에 옮깁니다.

| Immune System | CALM in RTPS |
| --- | --- |
| 사이토카인 농도 상승 | 재전송 backlog 증가 $\Delta \rho_r > 0$ |
| 신호 자체가 억제를 유도 | writer-local backlog 변화가 억제 신호 |
| 몸 전체를 멈추지 않고 반응 규모만 제한 | 애플리케이션 publish는 유지하고 재전송 budget만 조절 |
| 막힌 반응 경로부터 안정화 | 가장 오래된 미확인 샘플부터 재전송 |
| 반복 실패 신호 down-regulation | 샘플별 재시도 횟수 $N_{s,r}$ 기반 backoff |

## Core Algorithm

CALM은 writer 내부에서 reader마다 독립적으로 동작하는 closed-loop 제어기입니다. 매 cycle마다 세 단계를 수행합니다.

### 1. Observe

Writer는 각 reader에 대해 RTPS가 이미 유지하는 상태에서 두 값을 추적합니다.

- `rho_r(t)`: reader `r`이 아직 ack하지 않은 미확인 샘플 수
- `Delta rho_r`: backlog 변화량
- `N_s,r`: 샘플 `s`를 reader `r`에게 재전송한 횟수

이 값들은 모두 writer-local 상태이므로 패킷 형식이나 reader 구현을 바꾸지 않습니다.

### 2. Modulate

CALM은 reader별 재전송 budget `B_r`을 AIMD(Additive Increase / Multiplicative Decrease) 방식으로 조절합니다.

- `Delta rho_r > 0`: backlog가 증가 중이므로 `B_r`을 곱셈 감소한다.
- `Delta rho_r <= 0`: backlog가 안정 또는 감소 중이므로 `B_r`을 조금씩 증가시킨다.
- `N_s,r`이 큰 샘플은 재시도 간격을 늘려 반복 실패 샘플이 채널을 계속 두드리지 않게 한다.
- 해당 reader에서 `AckNack`/ack 진행이 관측되면 backoff를 완화하거나 reset한다.

### 3. Transmit

정해진 budget 안에서 가장 오래된 미확인 샘플부터 재전송합니다.

- gap이 있으면 재전송을 새 전송보다 우선한다.
- 새 샘플은 애플리케이션 발행을 막지 않고 writer buffer에 머문다.
- 앞선 gap이 해소되면 새 샘플 전송이 이어진다.

완전 신뢰성에서는 앞의 gap이 메워지기 전까지 뒤 샘플이 reader에 순서대로 전달될 수 없으므로, oldest-first 재전송은 in-order 완료 지연을 줄이는 방향입니다.

## Expected Properties

CALM은 평균 부하가 채널 용량 이하인 `lambda <= mu` 상황에서 다음을 목표로 합니다.

- **Lossless reliability:** 샘플을 의도적으로 버리지 않는다.
- **Source preservation:** 애플리케이션 publish rate를 throttle하지 않는다.
- **Storm suppression:** `Delta rho_r`가 양으로 발산하지 않도록 재전송 volume을 억제한다.
- **Low in-order latency:** 오래된 gap부터 메워 cumulative ack 전진을 빠르게 한다.
- **Reader isolation:** 나쁜 링크의 reader는 자기 budget만 줄어들며 다른 reader를 끌고 가지 않는다.
- **Wire compatibility:** RTPS packet format과 reader-side behavior를 바꾸지 않는다.

## Implementation Plan

CALM은 ROS 2 DDS middleware의 writer-side retransmission path에 구현하는 것을 목표로 합니다.

대상 middleware:

- Fast DDS
- Cyclone DDS

주요 hook 지점:

- writer-side retransmission scheduler
- per-reader `ReaderProxy` 또는 동등 상태 구조
- writer history/cache 및 재전송 큐

추가되는 상태:

- reader별 backlog `rho_r`
- reader별 backlog 변화 `Delta rho_r`
- reader별 재전송 budget `B_r`
- 샘플별 재시도 횟수 `N_s,r`
- 샘플별 backoff 상태

설계 원칙:

- application API 변경 없음
- reader-side 변경 없음
- RTPS wire format 변경 없음
- heartbeat cadence는 관측용으로 유지하고, 억제는 data retransmission budget에만 적용
- stock DDS와 mixed deployment 가능

## Evaluation Plan

CALM은 stock DDS, 정적 flow controller, TCP 기반 전송, ablation variant와 비교합니다.

Baselines:

- Stock Fast DDS / Cyclone DDS
- CALM without oldest-first retransmission priority
- CALM without AIMD budget control
- Fast DDS flow controller
- TCP transport with equivalent workload

Scenarios:

- WiFi lossy stream with 5%, 10%, 20% loss
- disconnect-reconnect recovery
- fan-out: one writer to multiple readers
- high-rate sensor stream such as IMU or LiDAR
- heterogeneous deployment with CALM and stock DDS endpoints
- ablation over AIMD parameters `alpha`, `gamma`

Metrics:

- retransmission volume peak
- convergence time `T_conv`
- reader별 `rho_r` 및 `Delta rho_r`
- in-order completion latency
- generated sample count vs delivered sample count
- application publish rate stability
- recovery time after loss spike or reconnect

## Research Contribution

이 프로젝트의 기여는 세 가지입니다.

1. ROS 2 DDS `RELIABLE` QoS의 재전송 폭풍이 source overload가 아니라 reliability loop 자체의 불안정성에서 발생할 수 있음을 정량적으로 분석한다.
2. writer-local backlog 변화와 샘플별 retry history만으로 재전송 volume을 조절하는 CALM 알고리즘을 제안한다.
3. Fast DDS와 Cyclone DDS의 RTPS 경로에 표준 호환 방식으로 구현하고, 무손실성, 지연, 폭주 수렴, reader 격리를 실험적으로 평가한다.

## Repository Structure

현재 연구 제안서 원문은 다음 위치에 있습니다.

- [`document/CALM_proposal.md`](document/CALM_proposal.md)

향후 구현이 진행되면 middleware patch, ROS 2 benchmark nodes, 실험 스크립트, 분석 notebook을 이 저장소에 추가할 예정입니다.

## Target Venue

Initial target venue:

- ACM/IEEE International Conference on Cyber-Physical Systems (ICCPS)

## Status

This repository is in the proposal and early implementation planning stage.

Current focus:

- formalizing the CALM control loop
- identifying Fast DDS and Cyclone DDS retransmission hook points
- designing repeatable WiFi loss and reconnect experiments
- defining lossless reliability and in-order latency metrics
