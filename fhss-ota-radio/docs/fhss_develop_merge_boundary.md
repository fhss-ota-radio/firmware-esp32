# FHSS와 Develop FSM의 Merge 경계

## 기준

- 확인한 원격 기준: `origin/develop` `3e3df61`
- 설계 기준: `docs/fsm-design.md`
- FHSS 통합 브랜치: `feature/fhss-radio-sync-integration`
- 진단 브랜치: `feature/fhss-sync-diagnostics`

## 결론

현재 FHSS 코드는 제품 FSM에 직접 연결하지 않고 독립 컴포넌트와 example로
분리한다. 최신 FSM은 애플리케이션 모드만 관리하고, FHSS 획득과 tracking은
무선 계층 내부 상태로 유지한다.

```text
제품 애플리케이션
main/fsm.*                      팀 공용, FHSS PR에서 수정하지 않음
    ↑ 후속 adapter가 LOST만 전달
components/fhss_service        FHSS 전용
    ↓
components/fhss_core
    ↓
components/rf_transport

고정 TX/RX 실기기 검증
examples/fhss_sync_test        제품 main과 완전 분리
```

## 지금 merge 가능한 범위

| 경로 | 판단 | 이유 |
|---|---|---|
| `components/fhss_core` | 가능 | 하드웨어와 전역 FSM에 독립적인 계산 계층 |
| `components/rf_transport` | 가능 | CC1101 전용 드라이버 |
| `components/fhss_service` | 조건부 가능 | 독립 컴포넌트로 merge 가능하지만 제품에서 자동 시작하면 안 됨 |
| `examples/fhss_sync_test` | 가능 | 고정 TX/RX 테스트 전용, 제품 FSM과 분리 |
| FHSS 관련 `docs` | 가능 | 구현과 검증 기록 |

## 이 PR에서 수정하면 안 되는 범위

| 경로 | 담당/위험 | 처리 |
|---|---|---|
| `main/fsm.c`, `main/fsm.h` | 팀 공용 FSM | develop 내용 그대로 유지 |
| `main/main.c` | 제품 통합 진입점 | 테스트 코드를 example로 이동 |
| `components/ptt_button` | 팀1 | watchdog 수정은 별도 담당자 PR |
| `components/rotary_encoder` | 팀1 | watchdog 수정은 별도 담당자 PR |
| `components/audio_*` | 팀1 | 음성 adapter 확정 전 수정 금지 |
| `components/display_ui` | 팀1 | 상태 표시 API 합의 후 연결 |
| `components/ota_client` | 팀2 | 라디오 mode ownership 합의 후 연결 |

## `fsm-design.md`와 맞춘 정책

1. `FSM_STATE_FHSS_SYNC`를 만들지 않는다.
2. `FSM_EVENT_SYNC_ACQUIRED`를 제품 FSM에 추가하지 않는다.
3. 부팅 후 제품 FSM은 `BOOT_INIT → MENU_IDLE`로 이동한다.
4. `SYNC_ACQUIRED`는 `fhss_fsm` 내부 상태로만 유지한다.
5. 완전히 추종을 놓쳤을 때만 기존 `FSM_EVENT_SYNC_LOST`를 전달한다.
6. OTA 수신·적용 중에는 음성 FHSS MISS를 세지 않는다.
7. 제품 단말은 고정 TX/RX 역할이 아니라 FSM 세션에 따라 역할을 바꿔야 한다.

## 아직 제품 FSM에 연결하면 안 되는 이유

현재 `fhss_service_start()`는 초기 설정에서 TX 또는 RX 역할 하나를 고정하고
태스크를 계속 실행한다. 반면 제품 FSM은 같은 단말에서 다음처럼 동작한다.

```text
MENU_IDLE → PTT_PRESS → TX_AUDIO
MENU_IDLE → RX_FRAME  → RX_AUDIO
MENU_OTA  → OTA_START → OTA_RECEIVING
```

따라서 production 연결 전에 다음 API가 필요하다.

- 음성 RX 대기 시작
- PTT에 따른 TX session 시작
- TX/RX session 정지
- 시작 채널 복귀
- OTA 진입 시 FHSS 정지 및 CC1101 ownership 반환
- OTA 종료 후 음성 RX 대기 재개
- 수신 음성 payload를 `fsm_post_rx_audio_frame()`으로 전달
- 완전한 tracking 상실만 `FSM_EVENT_SYNC_LOST`로 전달

## 검증

- 최신 develop merge 충돌 없음
- 제품 루트 전체 빌드 성공
- 독립 `examples/fhss_sync_test` 빌드 성공
- 최신 develop 기준 최종 diff에 `main`, PTT, 로터리, audio, display, OTA 변경 없음

