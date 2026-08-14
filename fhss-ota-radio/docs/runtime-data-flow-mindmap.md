# FHSS OTA Radio Firmware — 런타임 데이터 흐름과 마인드맵

> 기준: `feature/ota-rf-receive` (`70c8949`), 2026-08-14  
> 범위: 현재 제품 실행 경로, 독립 시험 경로, 아직 연결되지 않은 경계를 함께 표시한다.

## 상태 범례

| 표시 | 의미 |
|---|---|
| 🟢 제품 실행 | 루트 `app_main()`에서 초기화되어 실행됨 |
| 🔵 독립 검증 | 컴포넌트/예제에서 실행·검증됐으나 제품 미연결 |
| 🟡 구현 | 코드와 API는 있으나 제품 실행 또는 E2E 검증이 없음 |
| 🔴 단절 | 통합 코드, hardware loop, reboot/rollback 등이 없음 |

## A. 전체 아키텍처 마인드맵

```mermaid
%%{init: {"theme": "base", "themeVariables": {"primaryColor": "#1e3a5f", "primaryTextColor": "#ffffff", "primaryBorderColor": "#93c5fd", "secondaryColor": "#334155", "secondaryTextColor": "#ffffff", "secondaryBorderColor": "#a5b4fc", "tertiaryColor": "#475569", "tertiaryTextColor": "#ffffff", "tertiaryBorderColor": "#cbd5e1", "lineColor": "#cbd5e1", "textColor": "#ffffff"}}}%%
mindmap
  root((FHSS OTA Radio<br/>ESP32-S3))
    제품 실행 🟢
      app_main
        fsm_init
        task_stats
        INIT_DONE post
      제품 FSM
        BOOT_INIT
        MENU_COMM
        MENU_IDLE
        MENU_OTA
        TX_AUDIO
        RX_AUDIO
        OTA_RECEIVING
        OTA_APPLYING
        ERROR
      사용자 장치
        SSD1306 OLED
        PTT button
        Rotary encoder
        WS2812 LED
      Audio
        INMP441 I2S RX
        Speex narrowband
        MAX98357A I2S TX
    Radio와 FHSS 🔵
      rf_transport
        CC1101 SPI
        payload 최대 60B
        RSSI LQI CRC
        GDO0 timestamp Queue
      fhss_core
        hop sequence
        slot scheduler
        sync packet
        timing window
        sync state
      fhss_service
        TX 또는 RX 고정 role
        FreeRTOS Task
        diagnostics
        fhss_sync_test
    RF OTA 🔵
      ota_protocol submodule
        commit 9d2aa2b
        DISCOVER와 DISCOVER_ACK
        START DATA END
        ACK와 NACK
      ota_client
        RX Queue depth 16
        consumer Task
        session state
        5 chunk batch cache
        ota_writer
      검증
        DATA CRC16
        ESP image validation
        image SHA-256
        boot partition 변경
      ota_queue_copy_test
        실제 OTA partition write
        timeout NACK
        selective retry
        TEST_CSV
    제품 통합 단절 🔴
      BOOT_INIT RF init 없음
      ota_client init 없음
      CC1101 RX loop 없음
      RF send callback 없음
      Audio와 FHSS 연결 없음
      esp_restart 없음
      valid 확정과 rollback 없음
```

## B. 제품 부팅과 FSM

### B-1. 실제 부팅 순서

```mermaid
flowchart TD
  RESET["Reset / Power-on"] --> ROM["ESP32-S3 ROM bootloader"]
  ROM --> BL["ESP-IDF 2nd stage bootloader"]
  BL --> PT["partition table + otadata 확인"]
  PT --> APP["선택된 app 실행"]
  APP --> MAIN["app_main()"]

  MAIN --> FI["fsm_init()"]
  FI --> EQ[("FSM event Queue<br/>depth 16")]
  FI --> AQ[("RX audio Queue<br/>depth 4")]
  FI --> FT["fsm_task<br/>priority idle+3"]

  FT --> BI["on_enter_boot_init()"]
  BI --> DID["eFuse MAC 하위 3B<br/>device ID 로그"]
  BI --> HW["최초 1회 주변장치 init"]
  HW --> OLED["display_ui<br/>GPIO21/20"]
  HW --> LED["status_led<br/>GPIO38"]
  HW --> INPUT["PTT + rotary polling"]
  HW --> AUDIO["audio_codec + audio_io"]

  MAIN --> STATS{"runtime stats enabled?"}
  STATS -->|"yes"| TT["task_stats Task<br/>2초 간격 · priority idle+10"]
  MAIN --> INIT["post INIT_DONE"]
  INIT --> EQ
  EQ --> FT
  FT --> COMM["BOOT_INIT → MENU_COMM"]

  BI -. "현재 없음" .-> RF["rf_transport_init()"]
  BI -. "현재 없음" .-> OTA["ota_client_init/start_consumer"]

  classDef running fill:#dff5e1,stroke:#277a35,color:#163a1d
  classDef boot fill:#eeeeee,stroke:#707070,color:#222222
  classDef gap fill:#ffe0e0,stroke:#b42318,color:#3b1110
  class MAIN,FI,EQ,AQ,FT,BI,DID,HW,OLED,LED,INPUT,AUDIO,TT,INIT,COMM running
  class RESET,ROM,BL,PT,APP,STATS boot
  class RF,OTA gap
```

GPIO20은 native USB D+다. 제품 부팅에서 `display_ui_init()`이 GPIO20을 I2C SCL로 전환하므로 보드의 native `USB` 포트를 monitor에 사용하면 COM 포트가 사라질 수 있다. boot/runtime 증거 수집에는 USB-UART `COM` 포트를 우선 사용한다.

### B-2. 제품 FSM 전이

```mermaid
stateDiagram-v2
  [*] --> BOOT_INIT
  BOOT_INIT --> MENU_COMM: INIT_DONE

  MENU_COMM --> TX_AUDIO: PTT_PRESS
  TX_AUDIO --> MENU_COMM: PTT_RELEASE
  MENU_COMM --> RX_AUDIO: RX_FRAME
  RX_AUDIO --> MENU_COMM: RX_DONE

  MENU_COMM --> MENU_IDLE: select IDLE
  MENU_COMM --> MENU_OTA: select OTA
  MENU_IDLE --> MENU_COMM: select COMM
  MENU_IDLE --> MENU_OTA: select OTA
  MENU_OTA --> MENU_COMM: select COMM
  MENU_OTA --> MENU_IDLE: select IDLE

  MENU_OTA --> OTA_RECEIVING: OTA client STARTED
  OTA_RECEIVING --> OTA_APPLYING: OTA client APPLYING
  OTA_APPLYING --> BOOT_INIT: OTA client COMPLETED
  OTA_APPLYING --> MENU_OTA: verification FAILED

  ERROR --> BOOT_INIT: RETRY
```

전역 처리:

- `ERROR`: `BOOT_INIT`을 제외한 정상 상태에서 ERROR로 이동 가능
- `SYNC_LOST`: `BOOT_INIT`과 `ERROR` 이외의 상태에서 `MENU_COMM`으로 복귀
- 메뉴 변경: `MENU_COMM`, `MENU_IDLE`, `MENU_OTA` 사이에서만 허용
- `OTA_START`: `MENU_OTA`에서만 유효
- `PTT_PRESS`, `RX_FRAME`: `MENU_COMM`에서만 유효

OTA event adapter는 구현됐지만 제품에서 `ota_client_init()`에 등록하지 않았기 때문에 위 OTA 전이는 현재 실제품 실행 경로에서 발생하지 않는다.

또한 `OTA_VERIFY_OK` 또는 `RETRY`로 `BOOT_INIT`에 재진입한 뒤 `INIT_DONE`을 다시 post하는 코드는 없다. 현재 전이표 그대로 실제 이벤트가 들어오면 FSM은 `BOOT_INIT`에 머무르므로, OTA 성공 시 즉시 reboot할지 또는 재초기화 완료 이벤트를 다시 올릴지 결정해야 한다.

## C. Audio 데이터 흐름

### C-1. 송신

```mermaid
flowchart LR
  PTT["PTT polling<br/>GPIO1"] --> CB["on_ptt_event()"]
  CB --> EV["PTT_PRESS"] --> EQ[("FSM Queue")]
  EQ --> FSM["MENU_COMM → TX_AUDIO"]
  FSM --> BEEP["speaker enable<br/>beep<br/>disable"]
  FSM --> TASK["tx_audio Task<br/>stack 8192"]
  MIC["INMP441"] --> I2S["I2S RX"] --> PCM["160 × int16<br/>20ms · 8kHz"]
  TASK --> I2S
  PCM --> ENC["Speex NB encode"] --> FRAME["encoded frame≤64B"]
  FRAME -. "TODO" .-> FHSS["FHSS/rf_transport TX"]
  FHSS -.-> CC["CC1101"]

  classDef running fill:#dff5e1,stroke:#277a35,color:#163a1d
  classDef gap fill:#ffe0e0,stroke:#b42318,color:#3b1110
  class PTT,CB,EV,EQ,FSM,BEEP,TASK,MIC,I2S,PCM,ENC,FRAME running
  class FHSS,CC gap
```

### C-2. 수신

```mermaid
flowchart LR
  CC["CC1101"] -. "제품 RX loop 없음" .-> FHSS["RF/FHSS audio demux"]
  FHSS -. "호출자 없음" .-> POST["fsm_post_rx_audio_frame()"]
  POST --> AQ[("RX audio Queue<br/>depth 4")]
  POST --> EV["RX_FRAME → FSM Queue"]
  EV --> FSM["MENU_COMM → RX_AUDIO"]
  FSM --> TASK["rx_audio Task<br/>stack 8192"]
  AQ --> TASK --> DEC["Speex decode"] --> PCM["160 × int16"]
  PCM --> I2S["I2S TX"] --> AMP["MAX98357A"] --> SPK["Speaker"]
  TASK --> TO{"1초 동안 새 frame 없음?"}
  TO -->|"yes"| DONE["RX_DONE → MENU_COMM"]

  classDef running fill:#dff5e1,stroke:#277a35,color:#163a1d
  classDef gap fill:#ffe0e0,stroke:#b42318,color:#3b1110
  class POST,AQ,EV,FSM,TASK,DEC,PCM,I2S,AMP,SPK,TO,DONE running
  class CC,FHSS gap
```

### C-3. 임시 loopback

`LOOPBACK_ENABLE`이 현재 정의돼 있다. `MENU_IDLE`에서 PTT를 누르면 정식 FSM 전이 대신 `mic_test` Task가 mic → Speex encode/decode → buffer에 저장하고 PTT release 뒤 재생한다. 이 경로는 제품 음성 RF 경로가 아니다.

## D. RF Transport와 FHSS

```mermaid
flowchart LR
  subgraph RF["rf_transport"]
    PAY["상위 payload≤60B"] --> SEND["send_packet"]
    SEND --> LF["length prepend"] --> TXF["CC1101 TX FIFO"] --> STX["STX"]
    RXF["CC1101 RX FIFO"] --> RECV["receive_packet"]
    RECV --> OUT["payload + RSSI + LQI + CRC_OK"]
    GDO["GDO0 rising"] --> ISR["GPIO ISR"] --> TS["esp_timer timestamp"]
    TS --> TQ[("timestamp Queue<br/>depth 1 overwrite")]
  end

  subgraph TX["FHSS service TX"]
    SCHED["slot scheduler"] --> GUARD["guard 전 channel 변경"]
    GUARD --> SYNC["13B SYNC encode"] --> SEND
    TQ --> TXD["TX timestamp diagnostics"]
  end

  subgraph RX["FHSS service RX"]
    SEARCH["SEARCHING"] --> SCAN["channel scan"] --> RECV
    OUT --> CRC{"CRC_OK?"}
    CRC -->|"yes"| DEC["SYNC decode"] --> WIN["timing window"]
    WIN --> SUCC["연속 성공"] -->|"3회"| TRACK["TRACKING"]
    TRACK --> PRED["다음 slot/channel 예측"] --> MISS["timeout/CRC fail"]
    MISS -->|"5회"| LOST["SYNC_LOST"] --> SEARCH
    CRC -->|"no"| MISS
  end

  PRODUCT["제품 FSM"] -. "start/stop/role 통합 없음" .-> TX
  PRODUCT -. "start/stop/role 통합 없음" .-> RX

  classDef tested fill:#dcecff,stroke:#2166b1,color:#10243b
  classDef gap fill:#ffe0e0,stroke:#b42318,color:#3b1110
  class PAY,SEND,LF,TXF,STX,RXF,RECV,OUT,GDO,ISR,TS,TQ,SCHED,GUARD,SYNC,TXD,SEARCH,SCAN,CRC,DEC,WIN,SUCC,TRACK,PRED,MISS,LOST tested
  class PRODUCT gap
```

FHSS 경로는 `examples/fhss_sync_test`에서 실행된다. 제품 FSM에는 `fhss_service_init()`과 `fhss_service_start()` 호출이 없다.

## E. RF OTA 전체 흐름

### E-1. 목표 흐름과 현재 경계

```mermaid
flowchart LR
  GW["Gateway / Qt"] --> AIR["CC1101 RF"]
  AIR -. "제품 RF RX Task 없음" .-> RFRX["rf_transport_receive_packet()"]
  RFRX --> RP["rf_transport_rx_packet_t<br/>payload≤60B · CRC_OK"]
  RP --> BR["ota_rf_bridge_receive_once()"]
  BR --> SUB["ota_client_submit_packet()"]
  SUB --> Q[("OTA RX Queue<br/>depth 16 · 값 복사")]
  Q --> CT["ota_consumer Task"]
  CT --> PROTO["ota_protocol v0.2<br/>decode/encode"]

  PROTO --> DISC["DISCOVER<br/>MENU_OTA gate"]
  PROTO --> START["START<br/>target/session/size/SHA"]
  PROTO --> DATA["DATA<br/>CRC/sequence/length"]
  PROTO --> END["END<br/>size/chunks"]

  START --> WRBEGIN["esp_ota_begin"]
  DATA --> CACHE[("5-chunk cache<br/>received_mask")]
  CACHE -->|"complete, 최대 240B"| WRITE["esp_ota_write 1회"]
  END --> VERIFY["esp_ota_end"]
  VERIFY --> SHA["partition SHA-256 비교"]
  SHA --> BOOT["set boot partition"]

  DISC --> RESP["DISCOVER_ACK"]
  START --> RESP2["ACK/NACK"]
  DATA --> RESP2
  END --> RESP2
  CT --> TIMEOUT["missing seq TIMEOUT NACK"] --> RESP2
  RESP --> SENDCB["send_callback"]
  RESP2 --> SENDCB
  SENDCB -. "제품 callback 없음" .-> AIR

  BOOT --> READY["READY_TO_REBOOT"]
  READY -. "esp_restart 없음" .-> REBOOT["새 app boot"]
  REBOOT -. "valid/rollback 없음" .-> CONFIRM["mark valid 또는 rollback"]

  classDef tested fill:#dcecff,stroke:#2166b1,color:#10243b
  classDef implemented fill:#fff0ad,stroke:#9a7100,color:#2d2500
  classDef gap fill:#ffe0e0,stroke:#b42318,color:#3b1110
  class RP,SUB,Q,CT,PROTO,DISC,START,DATA,END,WRBEGIN,CACHE,WRITE,VERIFY,SHA,RESP,RESP2,TIMEOUT tested
  class BR,BOOT,READY implemented
  class GW,AIR,RFRX,SENDCB,REBOOT,CONFIRM gap
```

파란 노드는 컴포넌트 또는 `ota_queue_copy_test`에서 검증됐다. 빨간 연결은 제품에서 아직 없다.

### E-2. OTA client 상태

```mermaid
stateDiagram-v2
  [*] --> UNINITIALIZED
  UNINITIALIZED --> IDLE: ota_client_init
  IDLE --> RECEIVING: valid START
  RECEIVING --> RECEIVING: DATA / timeout NACK
  RECEIVING --> VERIFYING: complete END
  VERIFYING --> READY_TO_REBOOT: image + SHA valid
  VERIFYING --> ERROR: verify/write failure
  RECEIVING --> ERROR: write failure
  RECEIVING --> IDLE: abort
  ERROR --> IDLE: abort
```

### E-3. OTA packet 크기

```mermaid
flowchart TB
  BODY["rf_transport payload 최대 60B"]
  BODY --> START["START 49B<br/>session 4 + target 4 + size 4<br/>chunks 4 + SHA256 32 + type 1"]
  BODY --> DATA["DATA 최대 60B<br/>header 12 + payload 최대 48"]
  BODY --> END["END 13B"]
  BODY --> ACK["ACK/NACK 11B"]
  BODY --> DISC["DISCOVER 1B<br/>DISCOVER_ACK 7B"]
```

### E-4. Queue와 5청크 cache

```mermaid
flowchart LR
  LOCAL["rf_transport local RX buffer"] -->|"memcpy"| Q[("FreeRTOS Queue<br/>16 × packet≤60B")]
  Q --> DEC["consumer decode"]
  DEC --> CACHE[("Batch cache<br/>5 × 48B<br/>lengths + received_mask")]
  CACHE --> MISSING["missing_mask"]
  MISSING --> NACK["sequence별 timeout NACK"]
  CACHE --> COMPLETE{"required mask complete?"}
  COMPLETE -->|"no"| WAIT["다음 DATA 대기"]
  COMPLETE -->|"yes"| CONTIG["seq 순서의 연속 buffer"]
  CONTIG --> WRITE["esp_ota_write<br/>배치당 1회"]

  QNOTE["Queue 목적<br/>producer/consumer 분리"] --> Q
  CNOTE["Cache 목적<br/>out-of-order 재조립"] --> CACHE

  classDef tested fill:#dcecff,stroke:#2166b1,color:#10243b
  class LOCAL,Q,DEC,CACHE,MISSING,NACK,COMPLETE,WAIT,CONTIG,WRITE,QNOTE,CNOTE tested
```

### E-5. 선택 재전송 sequence

```mermaid
sequenceDiagram
  participant G as Gateway
  participant C as ota_consumer
  participant B as 5-chunk cache
  participant W as ota_writer

  G->>C: DATA seq=0
  C->>B: store 0
  C-->>G: ACK 0
  G->>C: DATA seq=2
  C->>B: store 2
  C-->>G: ACK 2
  G->>C: DATA seq=4
  C->>B: store 4
  C-->>G: ACK 4
  Note over C,B: timeout, missing_mask = seq 1 + seq 3
  C-->>G: NACK 1 TIMEOUT
  C-->>G: NACK 3 TIMEOUT
  G->>C: retry DATA seq=1
  C->>B: store 1
  C-->>G: ACK 1
  G->>C: retry DATA seq=3
  C->>B: store 3
  B->>W: contiguous seq 0..4, 최대 240B
  C-->>G: ACK 3
  G->>C: duplicate DATA seq=2
  Note over C: 이전 배치이므로 Flash 재기록 없음
  C-->>G: ACK 2 재전송
```

### E-6. 마지막 부분 배치

```mermaid
flowchart LR
  TOTAL["total_chunks = 13"] --> B0["base 0<br/>seq 0..4<br/>mask 0x1F"]
  B0 --> B1["base 5<br/>seq 5..9<br/>mask 0x1F"]
  B1 --> B2["base 10<br/>seq 10..12<br/>chunk_count 3"]
  B2 --> MASK["required_mask 0x07"]
  MASK --> DONE["seq 13,14를 기다리지 않음"]
```

### E-7. END 검증과 아직 없는 부팅 확정

```mermaid
sequenceDiagram
  participant C as ota_consumer
  participant O as ota_client
  participant W as ota_writer
  participant F as OTA partition
  participant B as Bootloader/New app

  C->>O: finish_session(session,size,chunks)
  O->>O: received_bytes와 expected_sequence 확인
  O->>W: finish(expected_sha256)
  W->>W: written_size == image_size
  W->>F: esp_ota_end
  W->>F: esp_partition_get_sha256
  W->>W: expected SHA-256 비교
  W->>F: esp_ota_set_boot_partition
  W-->>O: ESP_OK
  O-->>C: READY_TO_REBOOT + COMPLETED
  C-->>C: END ACK
  Note over C,B: 여기까지 구현·독립 검증
  C--xB: esp_restart 미구현
  B--xB: health check와 mark valid 미구현
  B--xB: invalid rollback 처리 미구현
```

## F. A/B Flash 배치

```mermaid
flowchart LR
  BL["bootloader / partition table<br/>0x000000..0x008FFF"] --> NVS["nvs<br/>0x009000 · 16KiB"]
  NVS --> OD["otadata<br/>0x00D000 · 8KiB"]
  OD --> PHY["phy_init<br/>0x00F000 · 4KiB"]
  PHY --> FACT["factory<br/>0x010000 · 1MiB"]
  FACT --> O0["ota_0<br/>0x110000 · 3MiB"]
  O0 --> O1["ota_1<br/>0x410000 · 3MiB"]
  O1 --> STORE["storage<br/>0x710000 · 960KiB"]
  STORE --> USED["0x800000<br/>앞쪽 8MiB 사용"]
  USED -.-> FREE["0x800000..0xFFFFFF<br/>16MiB Flash 상위 절반 미사용"]
```

## G. FreeRTOS producer–consumer 표

| 객체 | Producer/생성자 | Consumer | 현재 상태 |
|---|---|---|---|
| FSM event Queue, depth 16 | UI callback, `app_main`, OTA adapter | `fsm_task` | 🟢 제품 실행 |
| RX audio Queue, depth 4 | `fsm_post_rx_audio_frame()` | `rx_audio` | 🟢 Queue/consumer, 🔴 RF producer 없음 |
| RF timestamp Queue, depth 1 | GDO0 ISR | `fhss_service` | 🔵 FHSS 예제 |
| OTA RX Queue, depth 16 | `ota_client_submit_packet()` / bridge | `ota_consumer` | 🔵 OTA 예제, 🔴 제품 init 없음 |
| `fsm_task` | `fsm_init()` | — | 🟢 |
| PTT Task | `ptt_button_init()` | — | 🟢 |
| Rotary Task | `rotary_encoder_init()` | — | 🟢 |
| `tx_audio` | `TX_AUDIO` 진입 | — | 🟢 encode까지, 🔴 RF TX 없음 |
| `rx_audio` | `RX_AUDIO` 진입 | — | 🟢 decode/play, 🔴 RF source 없음 |
| `mic_test` | MENU_IDLE + PTT | — | 🟢 임시 기능 |
| `task_stats` | `app_main()` | — | 🟢 defaults 적용 시 |
| `fhss_service` | `fhss_service_start()` | — | 🔵 예제 |
| `ota_consumer` | `ota_client_start_consumer()` | OTA Queue | 🔵 예제 |

Batch cache는 Task도 Queue도 아니다. `ota_client_context_t`가 소유하는 고정 SRAM 구조체다.

## H. 제품 통합 시 필요한 최종 연결

```mermaid
flowchart TD
  OWNER["1. CC1101 단일 소유자 정의"] --> INIT["2. BOOT_INIT에서 RF/OTA init"]
  INIT --> RXLOOP["3. RF RX loop"]
  RXLOOP --> DEMUX{"4. packet mode/type 분류"}
  DEMUX -->|"MENU_COMM"| AUDIO["Audio/FHSS RX"]
  DEMUX -->|"MENU_OTA"| OTA["ota_rf_bridge → ota_consumer"]
  OTA --> TXRESP["5. ACK/NACK RF send callback"]
  AUDIO --> TXAUDIO["6. Speex frame FHSS/RF TX"]
  OTA --> RESTART["7. END ACK 후 restart"]
  RESTART --> HEALTH["8. 새 앱 health check"]
  HEALTH -->|"정상"| VALID["mark app valid"]
  HEALTH -->|"실패"| ROLLBACK["rollback and reboot"]
  VALID --> REENTRY["9. BOOT_INIT 재진입 정책 정리"]
  ROLLBACK --> REENTRY
  REENTRY --> E2E["10. Gateway–RF–ESP32 E2E 시험"]
```

## I. 구현 상태 요약

### 🟢 제품 실행

- 제품 FSM, FSM Queue, Audio Queue
- OLED, PTT, 로터리, WS2812
- device ID 로그
- I2S mic/speaker와 Speex encode/decode
- 임시 loopback 및 CPU runtime stats

### 🔵 독립 검증

- CC1101 transport와 FHSS service
- ota-protocol v0.2 decode/encode
- OTA consumer Task와 mode gate
- START/DATA/END ACK/NACK
- CRC, sequence, timeout, duplicate 처리
- 5청크 batch와 단일 Flash write
- 실제 OTA partition write와 잘린 이미지의 validation 실패
- abort 복구와 TEST_CSV collector

### 🟡 구현됐지만 제품 미연결

- `ota_rf_bridge`
- OTA event ↔ 제품 FSM adapter
- `main`의 `ota_client` compile dependency
- 전체 SHA-256 비교와 boot partition 설정의 정상 이미지 성공 경로

### 🔴 남은 작업

- 제품 CC1101/FHSS/OTA 초기화
- 실제 RF RX loop와 OTA response 송신
- Audio와 FHSS 연결
- radio mode arbitration
- `esp_restart()`
- 새 app valid 확정과 rollback
- `BOOT_INIT` 재진입 후 `INIT_DONE` 처리
- GPIO20/native USB 충돌 해결
- 전체 E2E/장애 주입 검증

## 근거 파일

- 부팅/FSM: `main/main.c`, `main/fsm.c`, `main/fsm.h`
- 제품 버전: `main/firmware_version.h`
- Audio/UI: `components/audio_io/*`, `components/audio_codec/*`, `components/display_ui/*`
- Device ID: `components/device_id/*`
- RF/FHSS: `components/rf_transport/*`, `components/fhss_core/*`, `components/fhss_service/*`
- OTA wire: `components/ota_protocol/upstream/include/ota_protocol.h`
- OTA 실행: `components/ota_client/source/*`
- RF bridge: `components/ota_rf_bridge/*`
- OTA 시험: `examples/ota_queue_copy_test/*`
- FHSS 시험: `examples/fhss_sync_test/*`
- Build/Flash: `dependencies.lock`, `sdkconfig.defaults`, `partitions.csv`
