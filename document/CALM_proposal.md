# Proposal

# CALM: Cytokine-Inspired Suppression of Retransmission Storms in ROS 2 DDS

### ROS 2 DDS의 재전송 폭풍을 면역계가 사이토카인 폭풍을 가라앉히듯 backlog 변화에 반응해 스스로 잠재운다. 완전 신뢰성을 유지하면서 지연을 낮춘다.

#### 투고 목표: ACM/IEEE International Conference on Cyber-Physical Systems(ICCPS)

---

# Abstract

 무선 환경의 ROS 2 로봇 통신에서 DDS의 RELIABLE QoS는 무선 손실 스파이크나 링크 단절-복구에서 재전송이 추가 부하가 되어 손실과 재전송이 눈덩이처럼 불어나는 retransmission storm에 빠진다. 평균 송신율이 채널 용량 안에 있어도(λ≤μ) 재전송 루프 자신의 불안정으로 발화한다. 본 연구는 사이토카인 폭풍을 멈추는 면역계의 방식, 즉 신호 농도의 상승에 비례해 자기 억제자를 유도하되 몸 전체는 멈추지 않는 음성 피드백에서 영감을 얻는다. 제안 기법 CALM(Concentration-Aware Loop Modulation)은 writer가 reader별로 재전송 backlog의 변화 *Δρ_r*와 재시도 *N_s,r*을 국소 추적해,

1. backlog 상승에 반응하는 closed-loop AIMD로 재전송 volume만 자가 억제하고
2. gap은 가장 오래된 미확인부터 메우며 새 전송은 그동안 대기시키고
3. 반복 실패한 샘플은 backoff한다.

 애플리케이션의 발행은 막지 않으며(새 샘플은 버퍼에 쌓였다 곧 전송된다), 패킷 형식도 수신측도 바꾸지 않는다. 평균 부하가 채널 용량 안에 있는 한, [무선 손실이 몰리는 구간]과 [링크가 끊겼다 복구되는 구간] 모두에서 폭주를 손실 없이 가라앉히고 데이터를 순서대로 빠르게 전달한다.

# I. 연구 배경 및 동기

## 1.1 재전송 폭풍 - 신뢰 전달 루프의 양성 피드백

 ROS 2는 통신을 DDS 미들웨어에 위임하고, DDS는 RTPS 프로토콜로 통신한다. Reliable QoS의 신뢰 전달은 “Heartbeat+AckNack+재전송”의 닫힌 루프다. Writer가 Heartbeat로 보유 시퀀스를 알리면 Reader가 AckNack으로 누락을 요청하고, Writer가 HistoryCache에서 재전송한다. 부하가 몰리면 이 루프가 스스로를 키우는 positive retransmission storm에 빠진다. 

1. 부하 폭주로 일부 메시지 손실
2. AckNack 지연 또는 누락
3. Writer가 Heartbeat 주기마다 재전송
4. 재전송 자체가 추가 부하가 되어 더 많은 손실, 더 많은 재전송

 핵심은 이 폭주가 평균 송신율이 채널 용량 안에 있어도(λ≤μ) 일어난다는 점이다. 원인은 소스 과부하가 아니라 재전송 루프 자신의 불안정이며, 두 상황에서 불이 붙는다.

**(L)** Lossy 링크 : 대역폭은 새 데이터에 충분하지만 손실 스파이크 한 번이 다수 NACK → 재전송 버스트→ 순간 채널 포화 → 추가 손실의 cascade를 켠다.

**(D)** 링크 단절 후 복구: 단절 동안 미확인 backlog가 쌓이고 재연결 순간 reader가 큰 범위를 NACK하면 writer가 전부 한꺼번에 재전송해 복구 시점에 폭주가 터진다.

외부 NS-3/ROS 2 평가 [[Lossy]](https://ieeexplore.ieee.org/document/9275849)은 손실 약 20%에서 RELIABLE 평균 지연이 약 5 ms에서 약 10 s로 치솟음을 보였다. 재전송은 본질적으로 reader별로 다르다. 어느 reader가 무엇을 놓쳤는지와 링크 상태가 모두 달라 폭주는 reader마다 따로 자란다. 

## 1.2 문제; 폭주를 멈추는 자가 억제가 없다

기존 RTPS가 재전송을 멈추는 수단은 세 가지뿐이고, 어느 것도 폭주를 손실 없이 부드럽게 가라앉히지 못한다.

| 억제수단 | 동작 | 한계 |
| --- | --- | --- |
| HISTORY 깊이(KEEP_LAST), lifespan | depth 초과, 만료 시 샘플을 history에서 제거 | 재전송이 멈추는 게 아니라 샘플을 버림(=손실).
완전한 신뢰성을 보장하지 않음. |
| 재전송 큐 상한
(Cyclone MaxQueuedRexmit) | 큐가 차면 초과 재전송 요청 무시 | 정적인 임계 설정.
무시된 요청은 잠재적으로 손실로 이어짐. |
| Heartbeat 주기시*(T_hb)* | 주기마다 재전송 점검 | 정적 파라미터라 부하 변동에 반응하지 않음 |

 흔히, “max-retransmits로 멈춘다”고 오해하지만, FastDDS/CycloneDDS에는 샘플당 재전송 횟수 상한이 없다. strict reliable(KEEP_ALL)에서는 ack 받을 때까지 사실상 무한 재전송한다. 즉, 손실 없이 재전송 rate를 부하에 맞춰 낮추는 자가 억제가 stock RTPS에는 아예 없다. 

 이와 별개로 일반 송신 rate를 제한하는 knob(Fast DDS flow controller)도 있으나, 이는 재전송만이 아니라 모든 트래픽을 정적으로 묶는 설정값이지 재전송 폭주에 반응하지 않는다. 요컨대 기존 knob은 정적 제한(History/Lifespan/재전송 큐 상한/Heartbeat 주기)이거나 일반 송신 rate 제한(flow controller)이다. 어느 것도 재전송 backlog가 자라는 것에 반응해 재전송량을 조절하지 않는다. CALM은 바로 이 backlog 증가에 반응한다.

 외부 혼잡 신호(ECN, BBR류)를 덧붙이는 시도는 (a) 신호 자체가 추가 부하가 되고 (b) 신호 도착 지연이 송수신 판단을 비대칭으로 만든다. 그래서 CALM은 외부 신호 없이 writer가 자기 backlog만 보고 억제한다. 

## 1.3 연구 질문과 범위

> 
> 
> 
> RESEARCH Question: RELIABLE 신뢰성은 손실 스파이크와 단절-복구 속에서도, **외부 신호 없이 스스로 가라앉을** 수 있는가?
> 

 CALM은 평균 부하가 채널 용량 안에 있는 (λ≤μ) RELIABLE 토픽에서 이 두 상황의 폭주를 끈다. 재전송 신호 자체가 브레이크가 되어 외부 신호 없이 재전송량을 가라앉힌다. 세 가지를 함께 지킨다.

1. 샘플을 버리지 않는 **무손실**
2. 애플리케이션 발행을 막거나 어떤 샘플도 버리지 않는 **소스 보존**
3. 막힌 구간을 먼저 매워 순서대로 가장 빨리 전달하는 **저지연**

 부하가 용량을 지속적으로 넘는 경우(λ>μ)는 어떤 무손실 기법으로도 지연을 묶을 수 없는 별개의 문제로, 의미 기반 능동 포기(SAR)가 다룬다.

## 1.4 자연(신경계)은 같은 폭주를 어떻게 멈추는가; 사이토카인 폭풍의 해결책

 이 양성 피드백 폭주는 우리 몸에도 똑같이 존재한다. 사이토카인 폭풍이다. 중증 감염(코로나19, 패혈증)에서 환자를 위협하는 것은 병원체가 아니라 면역계 자신의 폭주다 [[CRS]](https://www.sciencedirect.com/topics/biochemistry-genetics-and-molecular-biology/cytokine-storm). 면역 세포가 내보낸 신호 물질(사이토카인)이 주변 세포를 더 자극해서 더 많은 신호를 내보내고, 통제를 벗어나면 장기가 손상된다. RTPS 재전송 폭주와 구조가 똑같다.

 중요한 것은 건강한 몸에서는 이 폭주가 거의 일어나지 않는다는 점이다. 면역계에는 폭주를 스스로 가라 앉히는 내장 브레이크가 있고, 그 핵심 원리는 단순하다. 신호 물질이 많아질수록, 그 신호가 자기를 억누르는 억제 물질을 그 양에 비례해 더 많이 만들어낸다 [[SOCS-bio]](https://www.mdpi.com/2075-4418/15/22/2927). 신호가 셀수록 브레이크도 그만큼 세진다. 게다가 이 억제는 신호와 같은 길을 타기 때문에 따로 알리는 전령이 필요 없다. 브레이크가 엑셀과 같은 배선을 쓰는 원리와 같다. 제안하는 알고리즘 CALM의 “Concentration-Aware”가 바로 이 “신호 자체 브레이크”원리다. 본 연구는 이 자기 억제를 RTPS 재전송 루프에 옮긴다. 

> 
> 
> 
> 핵심 통찰: 사이토카인 폭풍은 브레이크의 부재가 아니라, 브레이크가 구동보다 느리거나 신호와 분리될 때 생기는 병이다. 
> 
> - RTPS 재전송 루프에는 이 자기 브레이크가 아예 없다.
>     - 재전송 트래픽이 자신의 *농도*(지금 얼마나 많은 샘플이, 얼마나 깊이 재전송을 요구하는지)와 그 *변화*를 어디에도 싣지 않기 때문이다.
> - CALM의 핵심은 이 누락된 자기 억제를 RTPS에 이식하되, 농도가 자라는 국면(*Δρ)*을 신호로 삼아 한 번에 재전송하는 샘플 수를 줄이는 것이다.
>     - 신호로는 backlog의 농도 변화 *Δρ*(억제 세기)와 샘플별 재시도 *N*(반복 실패 down-regulation)을 구분한다.

| 면역계의 해결책(실제 면역학) | CALM의 RTPS대응 |
| --- | --- |
| 사이토카인 농도의 상승(신호가 빠르게 느는 국면)
→억제 발동 | 재전송 backlog 변화량 *Δρ*(미확인 샘플 수가 자람=Δρ>0)
→ 자가 억제 발동 |
| 억제는 신호와 같은 길을 타서 별도의 전령이 없음 | *Δρ*는 writer가 자기 backlog로 국소 관측.(패킷/수신 측 변경 x) |
| 몸 전체를 멈추지 않고 반응 규모만 제한 | 애플리케이션 발행은 막지 않고, 한 cycle 재전송량(budget)만 AIMD로 억제 |
| 막힌 자리부터 해소 | 가장 오래된 미확인 샘플부터 재전송하고, 
풀릴 때까지 새 전송은 대기(in-order 완료 우선) |
| 반복 실패한 신호는 down-regulate | *N*이 큰(=여러번 실패한) 샘플의 재시도 주기를 backoff(그만함) |

 AIMD(Additive Increase/Multiplicative Decrease)는 혼잡 제어에서 널리 쓰이는 조절 규칙이다. 평상시엔 한 번에 보내는 양을 조금씩 더해 늘리고, 폭주 신호가 오면 곱셈으로 확 줄인다. 천천히 늘리고 빠르게 줄이는 비대칭 덕분에 한 번 잡힌 폭주가 쉽게 다시 불붙지 않고, 여러 독립 루프가 한 채널을 나눠 써도 공정하게 수렴한다 [Chiu-Jain]. CALM은 이 규칙으로 재전송 budget *B*를 조절한다. 

  *Δρ*와 *N* 모두 writer가 자기 backlog(재전송 대기 샘플 집합)로 국소 추적한다. 패킷에 아무것도 싣지 않고 수신측도 손대지 않는다. 억제는 backlog의 상승에 반응해 한 cycle 재전송량만 줄인다. 막힌 구간이 있으면 그 reader로의 새 전송은 미루고 재전송을 먼저 보낸다. 이 신호들이 CALM의 전부다. 모두 완전 신뢰성에서 동작하며, 샘플을 하나도 버리지 않고 재전송 폭주만 가라앉힌다. 

## 1.5 필요한 알고리즘의 3대 조건

| 조건 | 왜 필요한가 |
| --- | --- |
| **농도 변화 기반 자가 억제.**
외부 메시지 없이, backlog 상승률에 따라 재전송 수를 억제 | 외부 혼잡 신호는 추가 부하와 송수신 판단 비대칭을 낳는다. |
| **무손실+소스 보존+in_order 저지연.**
샘플을 버지리 않고 소스 rate도 늦추지 않으면서,
재전송 혼잡을 없애 전달을 빠르게 완료 | BEST_EFFORT는 신뢰성을 통째로 포기한다. 
신뢰성이 필요한 토픽엔 부적합하다. |
| **부하, 규모 무관 강건성.**
호스트 수 및 수신율이 변해도 동작한다. | 무선 로봇은 호스트 수 및 손실율이 수시로 변해서 고정된 튜닝이
통하지 않는다. |

세 조건 모두 λ≤μ의 완전한 신뢰성에서 만족한다. 

# 2. 연구 기여

1. ROS 2 RELIABLE QoS의 재전송 동작을 정량 해부하고, stock RTPS에는 손실 없는 자가 억제가 없음을 보인다. 재전송을 멈추는 수단은 History/Lifespan(샘플을 버려 손실(완전한 신뢰성 보장이라 보기 어려움))과 정적 *T_hb*(부하 무반응) 뿐이라, 부하 폭주에서 신뢰성의 취지를 위반함을 측정으로 보인다. 
2. CALM: writer가 reader 별로 재전송 backlog의 변화 *Δρ_r*와 샘플별 재시도 횟수 *N_s,r*을 국소 추적하여 
    1. backlog 상승에 반응하는 closed-loop AIMD로 재전송 volume(*budget B_*r)을 자가 억제하고
    2. gap이 있는 동안 가장 오래된 미확인 샘플부터 재전송하며 새 전송은 그 뒤로 미루어 in-order완료를 앞당기고
    3. 반복 실패한 샘플의 재시도를 backoff한다. 
    4. 이때, 애플리케이션의 발행은 막지 않는다(새 샘플은 잠시 버퍼에 머물다가 곧 전송된다).
        
        cf) TCP는 새 데이터와 재전송을 한 윈도우로 묶어서 손실이 나면 둘 다 줄임.
        
    
    무선 손실이 몰리는 구간(L)과 단절-복구 구간(D) 모두에서 폭주를 손실 없이 가라앉힌다. reader마다 독립 루프라 링크 나쁜 reader가 전체를 끌고 가지 못한다. 
    
3. CALM을 FastDDS, CycloneDDS의 RTPS 경로에 표준 호환 방식으로 구현하고, stock DDS/flow controller/동일 워크로드의 TCP 전송과 비교해 무손실/지연/수렴을 정량 평가한다. 

# 3. 선행 연구와 차별성

 바이오 영감 네트워킹은 오래전부타 있었다. TCP Symbiosis [[Symbiosis]](https://www.researchgate.net/publication/292027739_TCP_Symbiosis_Bio-Inspired_Congestion_Control_Mechanism_for_TCP) 류의 개체군 동역학, 군집(flocking), 이상탐지용 인공면역계 (AIS)가 대표적이다. 그러나 사이토카인의 자기 억제 음성 피드백을 재전송 자가 조절로 옮긴 사례는 없다. CALM은 바로 이 자리를 채운다. 

## 3.1 직접 기반 - 본 연구의 측정/모델

 “Heartbeat → AckNack → 재전송 루프를 이산 Markov 과정”으로 모델링한 확률적 지연 [[PLA]](https://arxiv.org/pdf/2508.10413)은 HisotryCache 깊이 *h_c*가 ‘메시지 크기에 선형/손실에 지수적으로’ 증가함을 보였다 (270 시나리오, MDR 오차 2% 미만). 분석적 지연 모델 [[LatModel]](https://ieeexplore.ieee.org/document/11044454)은 정상상태 성공확률 *q=p^3/(1-p^m+p^3)*을 유도하고 Heartbeat 주기 단축이 지연 감소에 약 50% 더 효과적임을 보였다. CALM은 이 *T_hb*를 억제 수단으로 늘리지 않고 빠른 관측용으로 유지하며, 억제는 이 모델들 위에서 데이터 토픽의 재전송 budget에 분리해 건다.

## 3.2 비교 기준 - NACK implosion과 부분 신뢰성 transport

 신뢰 멀티캐스트의 고전 문제는 다수 수신자가 동시에 NACK을 보내 송신자를 압도하는 feedback implosion이다. [[SRM]](https://ieeexplore.ieee.org/document/650139)은 NACK을 멀티캐스트하고 무작위 지연으로 중복을 억제하며, [[PGM]](https://www.rfc-editor.org/info/rfc3208/)은 라우터가 NACK을 집약한다. 모든 수신측 피드백 중복을 줄인다. 부분 신뢰성 transport인 [[PR-SCTP]](https://www.rfc-editor.org/info/rfc3758/)는 timed/limited reliability로 적시성과 신뢰성을 절충하고, [[QUIC datagram]](https://www.rfc-editor.org/info/rfc9221/)은 신뢰/비신뢰를 다중화한다. 모두 단일 end-to-end 연결 관점이다. 

→ 무엇을 빌려오는가?

- 면역계의 자기 억제에서 **“신호 자체가 곧 브레이크다”**를, NACK 억제에서 **“폭주는 피드백 구조의 문제다”**라는 시각으로 (대상을 송신측 재전송으로 뒤집어) 가져온다.

## 3.3 차별성

 CALM은 검증된 도구를 새로운 자리에 놓는다. backlog 변화에 반응하는 AIMD는 혼잡 제어에서, “폭주는 피드백 구조의 문제”라는 시각은 NACK 억제 연구에서 가져왔다. 새로운 점은 이 도구를 RTPS Reliability 루프 안의 재전송 흐름에 적용한다는 데 있다. 재전송 폭풍은 UDP 위 미들웨어 안에서 일어나므로 transport layer의 혼잡 제어로는 닿지 않고, RTPS 안에서 풀어야 한다. 

 혼잡 제어와 견주면 한 가지가 분명히 다르다. TCP는 새 데이터와 재전송을 한 윈도우로 묶어 손실이 나면 둘 다 줄이지만, CALM은 막힌 구간을 메우려 재전송을 새 전송보다 우선할 뿐 애플리케이션 발행 자체는 throttle하지 않고 샘플도 버리지 않는다. 막힌 구간이 풀리면 새 전송이 곧 이어진다. 

| 항목 | NACK 억제
(SRM/PGM) | 부분 신뢰성
(PR-SCTP) | TCP/QUIC 혼잡제어 | CALM |
| --- | --- | --- | --- | --- |
| 계층 | 신뢰 멀티캐스트 | transport | transport | RTPS Reliability
(UDP 위)(미들웨어 레이어)  |
| 조절대상 | 수신측 중복 NACK | 재전송 한계 | new+retransmit
(cwnd) → 새전송/재전송 모두 조절 | 재전송 volume만 조절 |
| 억제신호 | 멀티캐스트 NACK + 타이머 | 만료 | loss/RTT | 재전송 backlog 변화 *Δρ*
(writer-local) |
| 소스제어 | 해당 없음 | 해당 없음 | 혼잡 시 소스까지 줄임  | 재전송 우선.
(막힌 구간 동안 새 전송 보류)
새 발행 throttle/드랍 없음 |

## 3.4 더 넓은 선행 연구 지형

| 분야 | 대표 연구 | 한 일 | CALM과의 차이 |
| --- | --- | --- | --- |
| 신뢰 멀티캐스트 | SRM, [**[RMTP]**](https://app.notion.com/p/Proposal-388acccdb9fc804fa78cc7c984e274ca?pvs=21), PGM | 수신측 NACK 중복 억제 | 송신측 재전송 budget 
자가 억제. 부분 신뢰성 아님 |
| 부분 신뢰성
transport | PR-SCTP, QUIC-DG, [**[PRTaxonomy]**](https://app.notion.com/p/Proposal-388acccdb9fc804fa78cc7c984e274ca?pvs=21) | timed·limited reliability | transport 단일 연결.
(DDS x, 토픽 x, 면역 영감 x) |
| 능동 큐 관리
(AQM) | [**[CoDel]**](https://app.notion.com/p/Proposal-388acccdb9fc804fa78cc7c984e274ca?pvs=21), ECN | 지연 제어,
 혼잡 시 패킷 폐기 | host 큐/패킷 폐기
그에 반해, CALM은 reliability 내부 무손실
(재전송 budget만 제한) |
| Age of Inform | [**[AoI]**](https://app.notion.com/p/Proposal-388acccdb9fc804fa78cc7c984e274ca?pvs=21) | 신선도 최적화 | CALM의 목적은 완전 신뢰성 하에 폭주 억제로 AoI와는 직교 |
| DDS 적응형
QoS | online K-means QoS | runtime 에서 RELIABLE/BEST_EFFORT 전환 | 재전송 self-regulation의 종결 부재 및 토픽 단위의 이산적인 전환만 일어남 |
| DDS 내장 흐름
(재전송 knob. 기본) |   • Fast DDS 
     : flow controller 
  • Cyclone    
    : MaxQueuedRexmit | 정적 send-rate / 재전송 큐 상한 |   • 재전송 backlog 증가에 반응하는 closed-loop가 아님
  • 정적인 제한에 불과함
  • 일반 송신 rate을 제한함(초과분 손실로 이어짐)
  • reader 별로 적응하는게 아님 |

> 
> 
> 
> **BEST_EFFORT와의 경계:** CALM의 주요 영역은 신뢰성이 필요하면서 폭주가 나는 토픽이다. BEST_EFFORT는 신뢰성을 통째로 포기하므로 이 토픽엔 답이 아니다. CALM은 완전 신뢰성을 유지한 채 재전송 폭주만 끈다. 
> 
> cf) 의미가 허용하는 토픽에서 신뢰성을 부분 완화하는 축은 SAR이 다룬다. 
> 

# 4. 문제 정형화

- **시스템 모델.** 한 RELIABLE 토픽. 도메인 안에 Participant *M*, endpoint *E*, 패킷 전달율 *p*, Heartbeat 주기 *T_hb*. 재전송 상태는 reader 별로 둔다 (RTPS ReaderProxy). reader *r*, 샘플 *s*에 대해.
    - *N_s,r* — 샘플 *s*를 reader 에게 재전송한 횟수.
    - *ρ_r(t)* — reader *r*이 아직 ack 하지 않은 미확인 샘플 수 (=재전송 backlog)(단순 개수)
    - *Δρ_r*— *ρ_r*의 변화량.
    - *B_r(t)*— 한 cycle 에 reader 에게 재전송하는 샘플 수 (budget).
    - γ(<1)(감소), α(증가) — budget AIMD 의 감소·증가 계수.
- **목표.** λ≤μ에서 reader 마다 다음을 동시에 만족한다.
    - **무손실** — 모든 샘플을 끝내 전달한다 (의도적인 drop 없음)
    - **소스 보존** — 애플리케이션 발행을 막거나 버리지 않는다 (새 전송은 복구 뒤로 미룬다).
    - **storm 억제** — backlog 변화 *Δρ_r*가 양으로 발산하지 않는다.
    - **in-order 저지연** — reader의 cumulative ack이 가장 빨리 전진하도록 in-order 완료 지연을 최소화한다.

### **평가 지표**

- **storm 억제.** reader 별 *ρ_r x Δρ_r*의 **유계성** / 순간 재전송 **volume peak** / 폭주 수렴 시간 ***T_conv.***
- **in-order 완료 지연.** 샘플이 발행되어 reader에 **순서대로 전달되기까지** **시간의 분포**
- **소스 보존.** CALM on/off에서 애플리케이션 **발행 rate 변화**(가 없어야 함)
- **무손실.** 모든 토픽에서 전달하는 샘플 수 = 생성 샘플 수
- **적응.** **손실 스파이크** / 재연결 후 *Δρ_r→0*으로 걸리는 **시간**

# 5. 제안 알고리즘-CALM

 CALM은 writer 안에서 reader 마다 도는 닫힌 제어 루프다. 매 주기 **관측→조절→전송** 세 단계를 거치며, 모두 writer측에서 끝나 패킷 형식도 수신측도 바꾸지 않는다.

## 관측 — 무엇을 보는가

 writer는 각 reader에 대해 두 가지만 읽는다. 둘 다 RTPS가 이미 유지하는 per-reader 상태 (Reader Proxy)에서 가져오므로 패킷에 아무것도 더 싣지 않는다.

- **backlog의 증가세(*Δρ_r*)**
    - 아직 확인(ack)받지 못한 미확인 샘플이 지금 늘고 있는지 줄고 있는지.
    - 폭주가 커지는 국면인지 가라앉는 국면인지를 알려주는 신호다.
- **샘플별 재시도 횟수(*N_s,r*)**
    - 어떤 샘플을 몇 번 다시 보냈는데도 닿지 않았는지.
    - 그 reader의 링크가 그 샘플에 얼마나 불리한지를 나타낸다.

 payload 크기/조각 수/손실률을 따로 재지 않아도 그 영향은 backlog 증가세에 그대로 드러난다. 채널이 느려지면 backlog가 자라기 때문이다. 

## 조절 — 두 손잡이를 돌린다

- **재전송량(budget *B_r*)**
    - 한 주기에 그 reader로 다시 보낼 샘플 수다.
    - backlog가 자라면(*Δρ_r*>0) 곱으로 확 줄이고, 멈추거나 줄면 조금씩 늘린다 ~> AIMD 방식
    - 손잡이에 고정된 상/하한을 두지 않아 reader 수나 손실률이 변해도 알아서 맞춰진다.
- **재시도 주기(*N_s,r* 기반 backoff)**
    - 같은 샘플이 여러 번 실패하면 그 샘플의 재시도 간격을 늘려 죽은 링크를 매번 두드리지 않는다.
    - 그 reader에서 응답(ACKNACK)이 돌아오면 링크가 살아났다는 뜻이므로, 즉시 원상복구한다.

## 전송 — 무엇을 먼저 보내는가

 정해진 budget을 어떤 샘플로 채울지가 마지막 단계다. 순서는 늘 재전송이 새 전송보다 먼저다. 그 reader에 막힌 구간(미확인 샘플)이 있으면 가장 오래된 것부터 budget을 채워 재전송하고, 새 샘플 전송은 그 구간이 풀릴 때까지 보류한다. 완전 신뢰성에서 새 샘플은 앞의 구멍이 메워지기 전엔 어차피 수신측에 순서대로 전달되지 못하므로, 구멍을 먼저 메우는 편이 전체 전달을 가장 빨리 끝낸다. 또한, 애플리케이션의 발행 자체는 막지 않는다; 새 샘플은 잠시 버퍼에 머물다 구간이 풀리는 즉시 이어 나간다. 

## 영향 — 무엇이 달라지는가

- 폭주가 스스로 멈춘다.
    - backlog가 자라면 재전송량이 줄고→채널 부하가 줄고→손실이 줄어 backlog가 더 안 자란다.
    - 음의 피드백이라 평형(*Δρ_r≈0*)에서 멈춘다.
- 전달이 빨라진다.
    - 막힌 구간을 가장 먼저 메우므로 수신측이 순서대로 받는 시점이 앞당겨진다.
- 나쁜 reader가 격리된다.
    - 각 reader의 루프는 독립이라, 링크가 나쁜 reader는 자기 budget만 줄 뿐 다른 reader의 몫을 침범하지 않는다.
    - 한 채널을 여럿이 나눠 써도 대략 공정하게 수렴한다.

이 루프가 앞의 두 상황을 그대로 받는다. 

- **(L) 손실이 몰릴 때**는 재전송량을 줄여 cascade를 끊으며 막힌 구간을 메우고, 용량에 여유가 있는 새 데이터는 구간이 풀리는 즉시 흐른다.
- **(D) 끊겼다 복구될 때**는 단절 동안 쌓인 backlog를 재시도 backoff로 묵혀 두었다가 응답이 돌아오면 재전송량을 천천히 올리며 누적분을 차근차근 흘려보내 복구 순간의 burst를 막는다.

## 이론 목표

1. **폭주 안정성**
    - 재전송량 조절이 음의 피드백이라 backlog가 양으로 발산하지 않는다.
2. **순서 완료 최소 지연**
    - 막힌 구간을 먼저 메우고 새 전송을 미루는 순서가, 같은 재전송량 아래에서 수신측 순차 전달을 가장 빨리 끝낸다.
3. **reader 격리/공정성**
    - 독립 루프라 나쁜 reader의 backlog가 다른 reader의 몫을 침범하지 못하고, 공유 채널에서 대략 공정하게 배분된다.

# 6. 실험 계획

- **플랫폼.**
    - Fast DDS 2.14 + Cyclone DDS 0.10 (양쪽 wire-level patch), ROS 2 Humble·Jazzy.
    - 관측은 ros2probe + tcpdump + Wireshark RTPS dissector.
- **베드.**
    - 랩탑 2~4 대를 WiFi 로 연결 (writer 1 대 + reader 1~3 대).
    - 규모는 2 → 3 → 4 대로만 키운다.
    - 실제 로봇 불필요.
    - 다수 reader 가 필요한 fan-out 은 랩탑마다 reader 노드를 여러 프로세스로 띄워 만들고, 손실은 인위 주입한다.
- **Baselinecadence는 주기인가요?**
    - **B1:** stock FastDDS/CycloneDDS
    - **B2:** budget AIMD만(재전송 우선 off, new interleave)
    - **B3:** oldest-first 재전송 우선만(budget 무제한)
    - **B4:** **CALM(budget+재전송 우선+N-backoff, 무손실)**
    - B-flow: =FastDDS flow controller(정적인 send-rate)
    - B-TCP: 동일 워크로드를 TCP 전송으로
    - 핵심 비교
        - B1 vs B4
        - B-TCP vs B4
            - 소스 throttle 유무/무손실
        - 보조 B2/B3 ablation으로 budget 억제와 재전송우선의 분리 기여.
- **시나리오**
    - S1/S2
        - 고부하 데이터 스트림 burst/scale-up(publish-rate상승/랩탑 2→3→4대 증가/대당 reader 노드 여러 프로세스)
        - *T-conv* 폭주수렴 곡선
        - budget 억제하는 효과 최대 (동시에 다수 재전송)
    - S3 lossy stream (Case L)
        - LiDAR over WiFi 5/10/20%
        - 손실 cascade에서 지연/goodput(모든 샘플 무손실 유지)
        - 손실 발행 rate이 CALM을 on/off한 상황에서 불변하는지를 확인할 것
    - S-D 단절-복구 (Case D)
        - 링크를 수 초동안 끊었다 재연결.
        - 복구 시점에서 재전송 burst가 paced drain되는지, 무손실/복구되는지 등 지연 측정
    - S4 fan-out
        - 1 writer x 다수 reader, reader를 나머지 랩탑에 분산, IMU 200 Hz
        - data plane storm, 높은 *ρ*
    - S5 strict cumulative (log topic)
        - CALM이 모든 샘플을 전달 (무손실 회귀 없음, 핵심 안전 지표로 사용)
    - S6 heterogeneous mix
        - CALM + stock 50/50
        - backward compatibility
    - S7 ablation
        - AIMD 계수 (γ, α) sweep / budget 억제 off (무제한 재전송) / 재전송 우선 off (new interleave)을 각각 비교함
    - S8 real ROS 2
        - Nav2, Moveit2 등을 활용하여 실험 진행
- **가설/통계**
    - B4가 Case L/D에서 샘플 손실을 0으로 하여 폭주를 가라 앉히고, in-order 완료 지연을 낮춘다.
        - 폭주를 가라 앉힘: 순간 재전송 volume, *T_conv*가 B1대비 유의미하게 감소
    - CALM on/off에서 애플리케이션 발행은 막히지 않고 전달 throughput도 그대로다.
    - TCP는 손실 시 소스까지 줄여서 발행이 throttle되기 때문에 CALM은 막지 않으므로, B-TCP 대비 유의미하게 좋은 결과
    - B2/B3 대비 B4 우위가 budget 억제와 재전송 우선 각각의 기여를 입증할 수 있음.

# 7. 구현 및 검증 과제

## 구현 계획

- **후킹 지점**
    - Writer 측 재전송 스케줄러에 reader 별 {*ρ_r x Δρ_r x N_s,r*}을 국소적으로 추적함(ReaderProxy)
        
        + budget *B_r* AIMD
        
        + gap 동안 oldest-first 재전송 & 새전송은 대기
        
        + N-backoff/AckNack reset
        
    - Heartbeat cadence는 건드리지 않는다.
    - 전부 writer 측이라 수신측/wire 변경이 없다.
    - FastDDS의 flow controller, CycloneDDS의 WHC 대상
- **투명성**
    - 애플리케이션/rclcpp API 무변경
    - mixed 환경에서 stock DDS와 그대로 상호운용됨 ~> CALM은 송신 스케줄만 바꿀 뿐 프로토콜은 표준 그대로 사용하기 때문에 그럼
    - *ρ_r / Δρ_r / N_s,r* 모두 writer 로컬이라 wire 변경 혹은 수신측 변경이 전혀 없다.

## 검증해야 할 기술 항목

1. 재전송 폭주 발생 지점
    - RELIABLE에서 writer 측 큐가 {HistoryCache / 비동기 publisher 큐 / 커널 소켓} 중 어디인지 검증
    - 농도 *ρ*를 어느 큐에서 집계할지 벤더별로 특정할 것
    - Wireshark를 사용해서 사전에 검증
2. rate/backoff 제어식
    - *ρ_r x Δρ_r* 정의
    - AIMD 계수 (γ, α)의 적정값 (고정 상/하한 없이  *Δρ_r*로  self-tuning할 수 있게) 정의
    - N-backoff의 증가 함수와 AckNack reset 조건 등 정의
    - 큐 신호만으로 추적하는 갱신식 (EWMA +control-theoretic) 구하기
3. History QoS 상호작용
    - CALM은 KEEP_LAST depth값과 무관하게 동작하도록 설계해야 함.
    - RELIABLE의 기본은 history가 차면 writer가 막히는 Block 모드이지 샘플을 버리는 Drop이 아니므로, depth가 얕아도 무손실이 깨지지는 않는다.
        - depth와 CALM의 budget/재전송 우선 스케줄링이 어떻게 겹치는지를 확인한다.
            - back-pressure와 budget 억제의 상호작용을 확인한다
4. reader 별 루프의 상호작용
    - 공유 무선 채널에서 다수 per-reader AIMD 루프가 실제로 공정하게 수렴하는지, bad-reader 격리가 성히바는지를 실측한다.
    - reader 수 증가 시 집합 안정성과 오버헤드를 측정한다.