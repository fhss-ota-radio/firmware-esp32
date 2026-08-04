# 무선기 단말 통합 FSM 설계

ESP32-S3 무선기 단말(`firmware-esp32`)의 최상위 애플리케이션 상태기계 설계 문서. FHSS 음성 통신과 OTA 수신, PTT/OLED/마이크/스피커/로터리엔코더 UX를 하나의 상태기계로 통합한다.

## 한눈에 보기

| 상태 | 한 줄 요약 |
|---|---|
| `BOOT_INIT` | 부팅, 주변장치 초기화 |
| `FHSS_SYNC` | 호핑 동기 획득/재획득 중 |
| `MENU_IDLE` | 음성 대기 (기본 메뉴) |
| `MENU_OTA` | OTA 대기 (사용자가 로터리로 진입) |
| `TX_AUDIO` | 음성 송신 중 |
| `RX_AUDIO` | 음성 수신 중 |
| `OTA_RECEIVING` | OTA 청크 수신 중 |
| `OTA_APPLYING` | OTA 이미지 검증·적용 중 |
| `ERROR` | 복구 불가 오류 |

상세 설명은 [§2 상태](#2-상태-states), 전이 규칙은 [§4 상태 전이표](#4-상태-전이표) 참고.

## 결정 이력

| 날짜 | 결정 | 이유 / 트레이드오프 |
|---|---|---|
| 2026-08-04 | nRF24L01 폐기, **CC1101 하나**로 음성 FHSS + OTA 수신 겸용 | 두 칩이 같은 보드 위에 있어 이원화 실익 없음. 반이중 트랜시버라 음성·OTA 동시 수신 불가 → [§1 하드웨어 공유](#하드웨어-공유) |
| 2026-08-04 | 로터리 엔코더로 `MENU_IDLE`/`MENU_OTA` **수동 선택**, 활동 중 전환 잠금 | 의도치 않은 OTA 수신/플래싱 방지가 우선. 트레이드오프: "1:N 동시 업데이트"는 각 단말이 사전에 `MENU_OTA`로 전환돼 있어야 성립(운영 절차 전제) → [§1 메뉴 게이팅](#메뉴-게이팅) |

## 1. 설계 전제

#### 하드웨어 공유
- 음성(FHSS)과 OTA가 물리적으로 같은 라디오(CC1101, 단일 트랜시버)를 공유한다. 하나의 반이중(half-duplex) 트랜시버가 동시에 음성 홉 수신과 OTA 브로드캐스트 수신을 둘 다 할 수 없다는 **하드웨어 제약**이다.
- `OTA_RECEIVING` 진입 시 CC1101은 음성 호핑 스케줄을 이탈해 OTA 수신 채널로 재동조(retune)하며, 그동안은 음성 송수신이 물리적으로 불가능하다.

#### 메뉴 게이팅
- 메뉴 모드(`MENU_IDLE`/`MENU_OTA`)는 수신 해석을 게이팅하는 최상위 상태이며, 활동 중(음성 송수신/OTA 수신·적용)에는 전환할 수 없다.
- 로터리 엔코더 클릭으로 메뉴를 확정하는 이벤트(`EV_MENU_SELECT_IDLE`/`EV_MENU_SELECT_OTA`)는 `MENU_IDLE`↔`MENU_OTA` 사이에서만 정의되어 있고, `TX_AUDIO`/`RX_AUDIO`/`OTA_RECEIVING`/`OTA_APPLYING` 상태에는 이 전이 자체가 없다. 즉 "메뉴 변경 불가"는 **전이표에 해당 이벤트를 정의하지 않는 것만으로 자연히 보장**된다 (해당 상태에서 이 이벤트가 들어오면 그냥 무시).
- 로터리 회전(커서 이동)은 FSM 이벤트가 아니다. 화면에 `MENU_IDLE`/`MENU_OTA` 중 무엇이 하이라이트되는지는 `display_ui` 컴포넌트 내부의 로컬 상태일 뿐이며, 클릭(확정) 시점에만 그때 하이라이트돼 있던 항목에 해당하는 이벤트가 FSM에 올라온다.

#### 동기화 상태 (`FHSS_SYNC`)와 에러
- `FHSS_SYNC` 상태 ≠ 홉 타이밍 보정. `FHSS_SYNC`는 "아직 동기가 없는" 두 경우에만 존재한다: (1) 최초 부팅 후 최초 동기 획득, (2) 연속 수신 실패로 동기를 완전히 잃어 재획득(재탐색)이 필요한 경우. 일단 동기가 잡히면(`EV_SYNC_ACQUIRED`) 이후의 타이밍 유지는 **별도 태스크가 계속 도는 방식이 아니라, 매 수신 패킷 검증에 성공할 때마다 그 자리에서 보정하는 이벤트 기반(event-driven) 방식**이다. 자세한 내용은 [§1.1](#11-동시성-모델-fhss-홉-추종-vs-최상위-fsm) 참고.
- `EV_SYNC_LOST`는 위 (2), 즉 "기대되는 수신 윈도우에서 연속 N회 유효 패킷을 받지 못함(검증 실패 포함)"으로 판정될 때만 발생하는 전역(global) 이벤트다. 이 경우에는 현재 어떤 최상위 상태에 있든(설령 `RX_AUDIO` 도중이라도) 통신 자체가 불가능해지므로 강제로 `FHSS_SYNC`로 전이한다.
- 미수신 카운팅은 `OTA_RECEIVING`/`OTA_APPLYING` 동안 정지한다. CC1101이 이 두 상태에서는 애초에 의도적으로 음성 호핑 스케줄을 벗어나 OTA 채널을 듣고 있으므로, 그 사이 음성 슬롯을 못 받는 것은 "동기 상실"이 아니다. `OTA_APPLYING`에서 음성 상태(`MENU_IDLE` 등)로 복귀할 때 CC1101은 경과 시간을 계산해 호핑 스케줄상 현재 있어야 할 슬롯으로 바로 재합류하며, 이 재합류가 실패해야만(연속 미수신) 비로소 `EV_SYNC_LOST`가 발생한다.
- 치명적 오류(`EV_ERROR`)도 전역 이벤트로, 어느 상태에서든 `ERROR` 상태로 강제 전이한다.

### 1.1 동시성 모델: FHSS 홉 추종 vs 최상위 FSM

이 문서의 상태기계(§2~§4, `fsm.c`)는 **애플리케이션 동작 모드**만 표현한다. 홉 타이밍 유지는 별도의 상시 실행 태스크가 프리러닝 타이머로 도는 것이 **아니라**, CC1101 수신 경로(패킷 핸들러) 안에 내장된 로직이다:

1. 매 홉 슬롯마다 패킷을 수신 시도한다.
2. **패킷이 도착해 CRC/주소 검증에 성공하면**, 그 즉시 그 패킷의 실제 도착 시각을 기준으로 다음 홉 타이머(슬롯 시작 시각)를 재정렬한다 — 이게 곧 "동기 유지"다. 이 보정은 audio 프레임이든 keepalive/비콘성 패킷이든, 유효하게 검증되기만 하면 매번 일어난다.
3. **패킷 검증에 실패하거나 아예 수신되지 않으면**, 내부 카운터만 증가시키고 이전 타이밍을 그대로 유지(추정)한다.
4. 이 카운터가 임계값(연속 N회 미수신/검증 실패)을 넘기면 그때 비로소 "동기 완전 상실"로 판정하고 `EV_SYNC_LOST`를 올려 `FHSS_SYNC`(재탐색) 상태로 강제 전이한다.

즉 보정 자체는 `MENU_IDLE`/`RX_AUDIO` 등 수신이 일어나는 모든 상태에서 수신 이벤트에 얹혀 자연히 일어나며, FSM은 이를 이벤트로 인지하지 않는다 — FSM이 신경 쓰는 것은 "동기가 있다(`EV_SYNC_ACQUIRED` 이후)"와 "완전히 끊겼다(`EV_SYNC_LOST`)" 두 가지뿐이다.

```mermaid
sequenceDiagram
    participant Radio as CC1101 수신 드라이버
    participant App as 상위 FSM (fsm.c)

    Note over Radio: 매 홉 슬롯마다 수신 시도
    Radio->>Radio: 패킷 CRC/주소 검증 성공
    Radio->>Radio: 도착 시각 기준 홉 타이머 보정 (동기 유지)
    Radio-->>App: (audio 프레임이면) EV_RX_FRAME

    Note over Radio: 다음 슬롯에서 검증 실패/무수신
    Radio->>Radio: 미수신 카운터 증가 (타이밍은 이전 값 유지)

    Note over Radio: 연속 N회 실패 누적
    Radio-->>App: EV_SYNC_LOST
    App->>App: 어떤 상태에 있었든 FHSS_SYNC로 강제 전이
```

## 2. 상태 (States)

| 상태 | 설명 | 담당(스펙 기준) |
|---|---|---|
| `BOOT_INIT` | 전원 인가 직후. I2S(마이크/스피커), OLED, 로터리 엔코더, SPI(CC1101), PTT 버튼 GPIO 초기화 | 팀1, 팀2 |
| `FHSS_SYNC` | 피어/네트워크와 주파수 호핑 동기 **획득/재획득**. 최초 부팅 시, 또는 연속 수신 실패로 동기를 완전히 잃었을 때만 진입한다. 동기 유지(타이밍 보정)는 이 상태가 아니라 [§1.1](#11-동시성-모델-fhss-홉-추종-vs-최상위-fsm)처럼 매 수신 패킷 검증 성공 시 이벤트 기반으로 이루어진다 | 팀5 |
| `MENU_IDLE` | 동기 완료, **음성 모드**로 호핑 시퀀스를 따라가며 PTT 입력 또는 음성 수신 프레임 대기. 기본(default) 메뉴. 수신 패킷은 음성으로 해석됨 | 팀1, 팀5 |
| `MENU_OTA` | 동기 완료, **OTA 대기 모드**. 수신 패킷은 펌웨어 청크로 해석되며 ACK/재전송 로직 수행 대상이 됨. 로터리 엔코더로 사용자가 명시적으로 진입해야 함(자동 진입 없음) | 팀1, 팀2 |
| `TX_AUDIO` | PTT 눌림 (`MENU_IDLE`에서만 발생). 마이크 캡처 → Opus 인코딩 → FHSS 채널 송신 | 팀1, 팀2 |
| `RX_AUDIO` | 상대 단말로부터 음성 프레임 수신 중 (`MENU_IDLE`에서만 발생) → Opus 디코딩 → 스피커 재생 | 팀1, 팀2 |
| `OTA_RECEIVING` | (`MENU_OTA`에서만 발생) 게이트웨이가 CC1101로 브로드캐스트한 OTA 청크 수신·버퍼링 | 팀2 |
| `OTA_APPLYING` | 수신 완료된 이미지 검증(체크섬) 및 OTA 파티션 기록, 재부팅 대기 | 팀2 |
| `ERROR` | 복구 불가 수준의 하드웨어/통신 오류. 로깅 후 재시도 또는 재부팅 대기 | 팀1(PM) |

## 3. 이벤트 (Events)

| 이벤트 | 발생 주체 | 설명 |
|---|---|---|
| `EV_INIT_DONE` | 부팅 시퀀스 | 주변장치 초기화 완료 |
| `EV_SYNC_ACQUIRED` | CC1101 수신 태스크 (홉 로직, 팀5) | 호핑 동기 획득 |
| `EV_SYNC_LOST` | CC1101 수신 태스크 (홉 로직, 팀5) | 호핑 동기 상실 (전역) |
| `EV_MENU_SELECT_IDLE` | 로터리 엔코더 클릭 핸들러 | `MENU_OTA`에서 클릭 시점에 `MENU_IDLE`이 하이라이트돼 있었으면 발생 |
| `EV_MENU_SELECT_OTA` | 로터리 엔코더 클릭 핸들러 | `MENU_IDLE`에서 클릭 시점에 `MENU_OTA`가 하이라이트돼 있었으면 발생 |
| `EV_PTT_PRESS` / `EV_PTT_RELEASE` | PTT 버튼 ISR/디바운스 태스크 | 송신 시작/종료 (`MENU_IDLE`에서만 유효) |
| `EV_RX_FRAME` | CC1101 수신 태스크 | 음성 프레임 도착 (`MENU_IDLE` 상태에서 수신 시) |
| `EV_RX_DONE` | 오디오 태스크 | 수신 무음 타임아웃 등으로 수신 종료 |
| `EV_OTA_START` | CC1101 수신 태스크 | 게이트웨이 OTA 헤더 패킷 감지 (`MENU_OTA` 상태에서 수신 시) |
| `EV_OTA_CHUNK` | CC1101 수신 태스크 | OTA 이미지 청크 수신 |
| `EV_OTA_COMPLETE` | CC1101 수신 태스크 | 마지막 청크 수신, 전체 이미지 확보 |
| `EV_OTA_VERIFY_OK` / `EV_OTA_VERIFY_FAIL` | OTA 적용 로직 | 체크섬/서명 검증 결과 |
| `EV_ERROR` | 임의 모듈 | 치명적 오류 (전역) |
| `EV_RETRY` | 사용자 조작 또는 워치독 | ERROR 상태에서 재시도 |

## 4. 상태 전이표

| 현재 상태 | 이벤트 | 다음 상태 | 비고 |
|---|---|---|---|
| `BOOT_INIT` | `EV_INIT_DONE` | `FHSS_SYNC` | |
| `FHSS_SYNC` | `EV_SYNC_ACQUIRED` | `MENU_IDLE` | 기본 메뉴는 항상 `MENU_IDLE`로 진입 |
| `MENU_IDLE` | `EV_PTT_PRESS` | `TX_AUDIO` | |
| `MENU_IDLE` | `EV_RX_FRAME` | `RX_AUDIO` | |
| `MENU_IDLE` | `EV_MENU_SELECT_OTA` | `MENU_OTA` | 로터리 클릭으로 메뉴 전환 |
| `MENU_OTA` | `EV_MENU_SELECT_IDLE` | `MENU_IDLE` | 로터리 클릭으로 메뉴 전환 |
| `MENU_OTA` | `EV_OTA_START` | `OTA_RECEIVING` | CC1101이 OTA 채널로 재동조, 음성 호핑 이탈 |
| `TX_AUDIO` | `EV_PTT_RELEASE` | `MENU_IDLE` | |
| `RX_AUDIO` | `EV_RX_DONE` | `MENU_IDLE` | |
| `OTA_RECEIVING` | `EV_OTA_CHUNK` | `OTA_RECEIVING` | self-loop, 버퍼 적재 |
| `OTA_RECEIVING` | `EV_OTA_COMPLETE` | `OTA_APPLYING` | |
| `OTA_APPLYING` | `EV_OTA_VERIFY_OK` | `BOOT_INIT` | 이미지 스왑 후 재부팅 |
| `OTA_APPLYING` | `EV_OTA_VERIFY_FAIL` | `MENU_OTA` | 이미지 폐기, 기존 펌웨어 유지, 재시도 위해 OTA 대기 모드 유지 (※ 확정 필요 — `MENU_IDLE`로 되돌릴지 재검토 가능) |
| **모든 상태** | `EV_SYNC_LOST` | `FHSS_SYNC` | 전역 전이 |
| **모든 상태** | `EV_ERROR` | `ERROR` | 전역 전이 |
| `ERROR` | `EV_RETRY` | `BOOT_INIT` | |

`TX_AUDIO`/`RX_AUDIO`/`OTA_RECEIVING`/`OTA_APPLYING` 상태에는 `EV_MENU_SELECT_IDLE`/`EV_MENU_SELECT_OTA`에 대한 전이가 **정의되어 있지 않다** — 활동 중 메뉴 변경 이벤트가 들어와도 무시된다는 뜻이며, 이것이 곧 "SW적으로 메뉴 변경 불가" 요구사항의 구현이다. 같은 이유로, 음성 통화 중(`TX_AUDIO`/`RX_AUDIO`)에는 애초에 `MENU_OTA`가 아니므로 `EV_OTA_START`가 발생해도(패킷이 음성으로 오인되거나 무시되므로) `OTA_RECEIVING`으로 끼어들 수 없다 — 이전 설계에 있던 "음성 통화를 OTA가 강제 인터럽트"하는 전이는 폐기됐다.

## 5. 상태 다이어그램

`fsm.c`에 실제 구현된 최상위 상태만 평면으로 그린 뷰. FHSS 홉 추종 병행 프로세스는 상태가 아니므로 여기 나타나지 않는다 (§1.1 참고).

```mermaid
stateDiagram-v2
    [*] --> BOOT_INIT
    BOOT_INIT --> FHSS_SYNC : EV_INIT_DONE
    FHSS_SYNC --> MENU_IDLE : EV_SYNC_ACQUIRED

    MENU_IDLE --> TX_AUDIO : EV_PTT_PRESS
    MENU_IDLE --> RX_AUDIO : EV_RX_FRAME
    MENU_IDLE --> MENU_OTA : EV_MENU_SELECT_OTA
    MENU_OTA --> MENU_IDLE : EV_MENU_SELECT_IDLE
    MENU_OTA --> OTA_RECEIVING : EV_OTA_START

    TX_AUDIO --> MENU_IDLE : EV_PTT_RELEASE
    RX_AUDIO --> MENU_IDLE : EV_RX_DONE

    OTA_RECEIVING --> OTA_RECEIVING : EV_OTA_CHUNK
    OTA_RECEIVING --> OTA_APPLYING : EV_OTA_COMPLETE

    OTA_APPLYING --> BOOT_INIT : EV_OTA_VERIFY_OK
    OTA_APPLYING --> MENU_OTA : EV_OTA_VERIFY_FAIL

    ERROR --> BOOT_INIT : EV_RETRY

    BOOT_INIT --> ERROR : EV_ERROR
    FHSS_SYNC --> ERROR : EV_ERROR
    MENU_IDLE --> ERROR : EV_ERROR
    MENU_OTA --> ERROR : EV_ERROR
    TX_AUDIO --> ERROR : EV_ERROR
    RX_AUDIO --> ERROR : EV_ERROR
    OTA_RECEIVING --> ERROR : EV_ERROR
    OTA_APPLYING --> ERROR : EV_ERROR

    MENU_IDLE --> FHSS_SYNC : EV_SYNC_LOST
    MENU_OTA --> FHSS_SYNC : EV_SYNC_LOST
    TX_AUDIO --> FHSS_SYNC : EV_SYNC_LOST
    RX_AUDIO --> FHSS_SYNC : EV_SYNC_LOST
    OTA_RECEIVING --> FHSS_SYNC : EV_SYNC_LOST
    OTA_APPLYING --> FHSS_SYNC : EV_SYNC_LOST
```

## 6. 구현 매핑

- 코드: [`main/fsm.h`](../main/fsm.h), [`main/fsm.c`](../main/fsm.c) — 테이블 기반 상태기계, FreeRTOS 큐로 이벤트 수신. **이 파일은 애플리케이션 동작 모드만 다루며, FHSS 홉 타이밍 보정 자체는 구현하지 않는다.**
  - **미반영 (TODO)**: 현재 코드의 `fsm_state_t`/`fsm_event_t`는 이 문서의 이전 버전(`FSM_STATE_IDLE` 단일 상태) 기준이다. `MENU_IDLE`/`MENU_OTA` 분리, `EV_MENU_SELECT_IDLE`/`EV_MENU_SELECT_OTA` 추가를 `fsm.h`/`fsm.c`/전이표 구현에 반영해야 한다.
- 음성 FHSS 홉 타이밍 보정과 OTA 수신은 **같은 CC1101 SPI 드라이버/태스크**(팀2+팀5 공동) 안에서 모드 전환으로 구현한다. 홉 타이밍 보정은 별도의 프리러닝 타이머 태스크가 아니라 **수신 이벤트 처리 로직에 내장**되며, 연속 N회 수신 실패로 동기를 완전히 잃었을 때만 `FSM_EVENT_SYNC_LOST`를, 재획득에 성공하면 `FSM_EVENT_SYNC_ACQUIRED`를 `fsm_post_event()`로 올린다. 정상적인 매 수신 성공은 FSM에 이벤트로 올라오지 않는다(암묵적으로 동기가 유지되고 있다는 뜻). `OTA_RECEIVING` 진입/이탈 시 이 태스크는 음성 호핑 스케줄 추종을 명시적으로 멈추고/재개한다. **수신 패킷을 음성/OTA 중 무엇으로 해석할지는 이 태스크가 현재 메뉴 모드(`fsm_get_state()`가 `MENU_IDLE`인지 `MENU_OTA`인지)를 참조해 결정한다.**
- 로터리 엔코더 태스크(신규, `components/rotary_encoder`, 팀1 담당 예정)는 회전 시 로컬 커서만 갱신(FSM 이벤트 없음), 클릭 시 그 시점 커서에 해당하는 `EV_MENU_SELECT_IDLE`/`EV_MENU_SELECT_OTA`를 `fsm_post_event()`로 올린다.
- 각 모듈(PTT 버튼 태스크, 로터리 엔코더 태스크, CC1101 수신 태스크, OTA 적용 로직)은 하드웨어 이벤트 발생 시 `fsm_post_event()`만 호출하고, 실제 상태 전이/부수효과는 FSM 태스크 하나에서만 처리한다 (경쟁 상태 방지).
- 상태 진입/이탈 시 수행할 하드웨어 동작(마이크 시작/정지, OLED 상태 표시 등)은 `fsm.c`의 `on_enter_*` 스텁에 각 담당 팀이 채워 넣는다.
