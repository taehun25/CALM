# S1: CALM 사전 실험 Baseline

S1은 CALM 알고리즘을 적용하기 전, 첫 번째 사전 실험을 위해 작성한
`calm_pretest` ROS 2 패키지의 현재 상태를 백업한 스냅샷이다.

## 목적

이 실험은 CALM 알고리즘을 적용하지 않은 상태에서 DDS 통신의
`RELIABLE`과 `BEST_EFFORT` 동작을 비교하기 위한 baseline 실험이다.

주요 목적은 무선 네트워크 환경에서 큰 payload를 전송할 때,
`RELIABLE` 통신의 재전송 과정이 수신 성능 저하로 이어지는지 확인하는
것이다. 이후 CALM 알고리즘 적용 결과와 비교하기 위한 기준 데이터를
만드는 것이 이 실험의 역할이다.

## 실험 구성

- 송신 노트북: `calm_automation`, `calm_pub` 실행
- 수신 노트북: SSH를 통해 `calm_sub` 실행
- 송신 측 ROS 2 workspace: `/home/csi/ros2_ws`
- 수신 측 ROS 2 workspace: `/home/csilab/ros2_ws`
- 기본 ROS domain ID: `117`
- 기본 RMW implementation: `rmw_fastrtps_cpp`
- 기본 History QoS: `KEEP_ALL`

## 실험 파라미터

S1의 기본 실험 matrix는 다음과 같다.

- reliability: `reliable`, `best_effort`
- payload: `32KB`, `64KB`, `128KB`, `256KB`, `512KB`
- publish Hz: `5`, `10`, `20`, `30`, `50`
- 반복 횟수: `5`

자동화 코드에는 짧은 디버깅 실행을 위한 override 옵션도 포함되어 있다.

```bash
ros2 run calm_pretest calm_automation \
  --remote-host <SUB_IP> \
  --payload-bytes 1024 \
  --hz 5 \
  --reliability reliable \
  --count 100 \
  --repeat 1 \
  --max-runs 1
```

## 패키지 위치

복사된 ROS 2 패키지는 다음 위치에 저장되어 있다.

```text
experiments/S1/calm_pretest/
```

주요 파일은 다음과 같다.

- `src/calm_pub.cpp`: 송신 노드
- `src/calm_sub.cpp`: 수신 노드
- `src/calm_automation.cpp`: SSH 기반 자동화 실행 코드
- `CMakeLists.txt`: 빌드 설정
- `package.xml`: ROS 2 패키지 메타데이터
- `README.md`: 패키지 단위 사용 설명

## 현재 구현 상태

- 각 run마다 `run_id` 기반의 고유 topic 이름을 사용한다.
- 송신자는 subscriber ready 신호와 data topic matching을 모두 확인한 뒤
  publish를 시작한다.
- 자동화 코드는 SSH로 수신 노트북의 `calm_sub`를 실행한다.
- 실험 결과는 `results/pretest_csv/` 아래에 CSV와 log 파일로 저장된다.
- S1에는 packet loss를 인위적으로 주는 기능은 구현되어 있지 않다.
- 현재 코드는 실제 관측된 수신율, 지연 시간, duplicate count, 수신 Hz를
  기록한다.

## 디버깅 기록

인터넷이 연결된 일반 Wi-Fi 환경에서 자동화 코드를 점검했을 때,
작은 payload는 정상 수신되었지만 큰 payload는 전달되지 않았다.

- `1KB`, reliable, `5Hz`, count `100`: `100/100` 수신
- `2KB`, reliable, `5Hz`, count `30`: `1/30` 수신
- `4KB`, reliable, `5Hz`, count `30`: `0/30` 수신
- `8KB`, reliable, `5Hz`, count `30`: `0/30` 수신
- `32KB`, reliable, `5Hz`, count `100`: `0/100` 수신

따라서 인터넷 연결용 Wi-Fi는 본 실험에서 사용할 큰 payload DDS 통신
실험에는 적합하지 않은 것으로 보인다. 실제 S1 baseline 실험은 두 노트북을
전용 실험 공유기에 연결한 상태에서 수행하는 것이 적절하다.
