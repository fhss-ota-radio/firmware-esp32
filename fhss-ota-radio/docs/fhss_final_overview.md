# FHSS 최종 구현 및 발표 개요

> 기준: `origin/develop` 2026-08-23. ESP32-S3 두 대와 CC1101 하나씩을 사용하며,
> 음성 통신과 OTA가 동일한 반이중 라디오를 공유한다.

## 1. 한눈에 보는 동작

```text
공통 rendezvous 채널에서 첫 SYNC 수신
        ↓
SYNC의 generation / slot / GDO0 timestamp 검증
        ↓
같은 seed와 slot로 TX/RX가 동일한 hop channel 계산
        ↓
300 ms 슬롯마다 CC1101 CHANNR 변경
        ↓
정상 패킷: 시간 오차를 제한적으로 보정
일시 단절: N / N-1 / N+1 제한 복구
연속 실패: SYNC_LOST 후 rendezvous 채널 재탐색
PTT 해제: END 패킷으로 정상 세션 종료
```

## 2. 채널 결정

`fhss_hop_sequence`는 허용 채널 배열을 seed 기반 deterministic permutation으로
섞는다. 암호화 알고리즘이 아니라, TX와 RX가 별도의 hopping table 전송 없이 같은
순서를 재현하기 위한 알고리즘이다.

기본 설정은 다음과 같다.

| 항목 | 값 |
|---|---:|
| 기본 seed | `OTA_FHSS_DEFAULT_SEED` (`0x46485353`) |
| 기본 후보 채널 | CC1101 `CHANNR 1~100` |
| 예약 채널 | `CHANNR 0` |
| rendezvous 채널 | 후보 배열의 첫 채널 |
| 기본 slot | `300,000 us` |

최신 develop에서는 활성 OTA FHSS 설정이 NVS에 저장되어 있으면 `generation`, seed,
첫 채널, 채널 수, slot 시간과 channel-switch guard를 그 설정에서 읽는다. NVS 설정이
없을 때만 위 기본값을 사용한다.

## 3. 시간 동기화

CC1101 GDO0 rising edge에서 `esp_timer_get_time()`을 기록한다. FIFO를 나중에 읽은
시각 대신 sync word가 검출된 시각을 사용하므로 FreeRTOS와 SPI 처리 지연의 영향을
줄인다.

| 설정 | 현재 값 | 목적 |
|---|---:|---|
| channel switch guard | 기본 `5,000 us` | 슬롯 전에 채널 변경 완료 |
| timing window | `±20,000 us` | 정상 수신 허용 범위 |
| correction deadband | `500 us` | 작은 jitter 무시 |
| slow correction | 오차의 `1/8` | 작은 지속 drift 보정 |
| fast correction | 오차의 `1/2` | 큰 지속 drift 보정 |
| correction max step | `500 us` | 단일 이상치의 기준 시각 훼손 방지 |

통제된 A/B 실험에서는 `+19 ms` timestamp 이상치를 주기적으로 넣었다. 보정 제한
전에는 10/10 이상치에서 SYNC_LOST가 발생했고, 500 us 제한 후에는 최소 16/16을
통과하며 180 슬롯 이상 TRACKING을 유지했다.

## 4. 연결 단절 복구

첫 수신 실패만으로 기준을 지우지 않는다.

```text
0~1회 MISS    → 기존 scheduler 예측 유지
2회 연속 MISS → RECOVERY 진입
RECOVERY      → 예상 N, N-1, N+1 슬롯 채널을 제한 탐색
5회 연속 MISS → hard re-search 및 SYNC_LOST
```

목표는 한 슬롯 정도의 slip이나 순간적인 scheduling 지연을 전체 채널 재탐색 없이
회복하는 것이다. 현재 300 ms 슬롯 기준 RECOVERY 진입은 이론상 약 600 ms이므로,
추후 200/100 ms slot A/B 실험으로 음성 재개 latency와 오류율의 균형을 찾아야 한다.

## 5. 음성 패킷과 정상 종료

- Speex narrowband: 8 kHz, mono, signed 16-bit PCM
- 160 samples / 20 ms frame
- 최대 Speex frame 두 개를 AUDIO packet 하나에 결합
- CC1101 payload 상한: 60 bytes
- 다음 슬롯 전 20 ms는 새 DATA 송신을 시작하지 않는 airtime guard

PTT를 놓으면 남은 AUDIO를 보낸 뒤 별도 type `0x03` END 패킷을 3회 전송한다.
RX는 중복 END를 한 번만 FSM에 전달하고 `RX_AUDIO → MENU_COMM`으로 정상 종료한 뒤
rendezvous 채널로 복귀한다. END가 모두 손실될 때만 무음 timeout/SYNC_LOST가
fallback으로 동작한다.

## 6. OTA와 FHSS

최신 develop은 OTA에도 동일 CC1101 FHSS를 적용한다.

```text
MENU_OTA
  → FHSS 설정 수신/저장
OTA_FHSS_CONFIGURED
  → generation 활성화 요청
OTA_FHSS_SYNCING
  → 새 generation SYNC 획득
OTA_FHSS_READY
  → OTA_RECEIVING
```

generation이 다른 설정을 섞어 적용하지 않으며, 활성 설정은 NVS에 보존된다. 음성과
OTA는 같은 라디오를 공유하므로 FSM 상태로 소유권과 모드 전환을 직렬화한다.

## 7. 발표 시연

### 정적 설명

[fhss_channel_hopping_demo.svg](fhss_channel_hopping_demo.svg)는 기본 seed와 기본
300 ms slot으로 계산한 처음 30개 hop 예시다. 알고리즘 원리를 설명하는 용도이며,
NVS에 다른 OTA FHSS 설정이 활성화되면 실제 순서와 다를 수 있다.

### 실제 통신 실시간 표시

`tools/fhss_live_channel_monitor.html`은 TX/RX serial log의 실제 slot/channel을
읽어서 두 선을 겹쳐 표시한다.

```powershell
python -m http.server 8000
```

Chrome/Edge에서 아래 주소를 열고 TX와 RX COM 포트를 각각 선택한다.

```text
http://localhost:8000/tools/fhss_live_channel_monitor.html
```

두 장치가 같은 slot/channel이면 `SYNCHRONIZED`, 다르면 `DIFFERENT`가 표시된다.
이 도구는 CC1101에 설정한 실제 CHANNR를 보여주지만 공중 RF 주파수를 계측하지는
않는다. 방사 신호 검증에는 스펙트럼 분석기가 필요하다.

## 8. 완료된 검증과 남은 검증

### 완료

- CC1101 PARTNUM/VERSION 및 register read-back
- 고정 채널 송수신과 CRC
- 3채널 synchronized hopping
- GDO0 timestamp 기반 시간 동기화
- 강제 단절 후 SYNC_LOST 및 재획득
- 19 ms 이상치에 대한 시간 보정 A/B 실험
- seed 기반 hopping 및 제한적 RECOVERY 구현
- 음성 세션 END 패킷 구현
- OTA FHSS 설정 생명주기 develop 반영

### 남음

- END 패킷 실제 도달률과 종료 latency 측정
- 장애 발생부터 첫 정상 AUDIO 재생까지 `audio_resume_ms` 측정
- 300/200/100 ms slot A/B 비교
- 2시간 이상 clock drift 및 recovery 성공률 측정
- 스펙트럼 분석기를 이용한 실제 주파수 이동 확인
- 거리·간섭 조건별 packet success rate와 음성 품질 측정

## 9. 문서 안내

| 문서 | 용도 |
|---|---|
| [fhss_algorithm_ab_test.md](fhss_algorithm_ab_test.md) | 시간 보정 A/B 수치 |
| [fhss_session_end_packet.md](fhss_session_end_packet.md) | 정상 세션 종료 프로토콜 |
| [fhss_sync_diagnostics.md](fhss_sync_diagnostics.md) | 진단 통계 |
| [fhss_radio_sync_integration.md](fhss_radio_sync_integration.md) | GDO0·scheduler·service 통합 |
| [fhss_audio_packetization.md](fhss_audio_packetization.md) | Speex/RF 패킷 구성 |
| [fhss_channel_hopping_presentation.md](fhss_channel_hopping_presentation.md) | 발표 및 실시간 시연 절차 |
| [troubleshooting/fhss/](troubleshooting/fhss/) | CC1101/FHSS 실기기 문제 해결 |
