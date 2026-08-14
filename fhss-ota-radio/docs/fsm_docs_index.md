# FSM 문서 안내

FSM 관련 문서가 제품 상태, FHSS 내부 동기 상태, 구현 시험으로 나뉘어 있어 목적에 따라 읽을 문서가 다르다.

## 권장 읽는 순서

| 순서 | 문서 | 역할 |
|---:|---|---|
| 1 | [무선기 단말 통합 FSM 설계](fsm-design.md) | 제품의 최상위 상태와 전이 기준 |
| 2 | [FHSS Service와 Main FSM 연동](fhss_service_fsm_integration.md) | 제품 FSM과 FHSS 내부 FSM의 책임 경계 |
| 3 | [FHSS 무선 시간 동기화](fhss_radio_sync_integration.md) | GDO0 timestamp, slot scheduler, synchronized hopping |
| 4 | [Speex 2-Frame 오디오 패킷화](fhss_audio_packetization.md) | 오디오 frame 두 개의 RF packet 변환과 FSM 전달 |
| 5 | [FHSS 동기화 진단 통계](fhss_sync_diagnostics.md) | clock drift, 채널별 성공률, timeout 진단 |

## 두 FSM을 구분하는 법

### 제품 Main FSM

- 코드: `main/fsm.c`, `main/fsm.h`
- 질문: 단말이 지금 통신, 뮤트, OTA, 송신, 수신 중 무엇을 하는가?
- 대표 상태: `MENU_COMM`, `TX_AUDIO`, `RX_AUDIO`, `OTA_RECEIVING`
- FHSS 정상 획득 이벤트는 소비하지 않고 완전한 추종 상실만 `FSM_EVENT_SYNC_LOST`로 받는다.

### FHSS 내부 FSM

- 코드: `components/fhss_service/fhss_fsm.*`
- 질문: 라디오가 지금 SYNC를 검색, 획득 중, 추종 중, 송신 중 무엇을 하는가?
- 대표 상태: `SEARCHING`, `SYNCHRONIZING`, `TRACKING`, `TRANSMITTING`
- 제품 UI 상태가 아니며 OLED 메뉴 상태로 직접 노출하지 않는다.

## 문서별 기준 범위

| 주제 | 기준 문서 |
|---|---|
| 제품 상태 이름·전이 | `fsm-design.md` |
| FHSS 내부 상태 전이 | `fhss_service_fsm_integration.md` 및 `fhss_fsm.c` |
| SYNC/slot/time 계산 | `fhss_radio_sync_integration.md` |
| 오디오 RF packet 형식 | `fhss_audio_packetization.md` |
| 통계와 장시간 drift | `fhss_sync_diagnostics.md` |
| 실제 하드웨어 시험 | `cc1101_433mhz_ping_test.md` |

## 현재 연결 상태

완료:

- 제품 FSM의 오디오 capture/decode 태스크
- FHSS SYNC 검색·추종·상실 내부 FSM
- CC1101 GDO0 timestamp
- Speex 2-frame RF packet pack/unpack
- `RX_AUDIO + RX_FRAME` 상태 유지 처리

연결 대기:

- TX audio capture callback → 2-frame aggregator → FHSS 송신
- FHSS RX audio packet → unpack → `fsm_post_rx_audio_frame()`
- 제품 메뉴 상태에 따른 FHSS/OTA 라디오 모드 전환
- `FHSS_SERVICE_EVENT_SYNC_LOST` → `FSM_EVENT_SYNC_LOST` adapter

결정 이력과 현재 구현이 다를 때는 `fsm-design.md`의 §4 전이표와 §6 구현 매핑, 실제 코드를 우선한다.
