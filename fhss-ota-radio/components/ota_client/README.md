# ota_client

`ota_client`는 CC1101을 통해 전달된 OTA 패킷을 검증하고, 새 펌웨어를 비활성 OTA 파티션에 기록한 뒤 부팅 파티션을 전환하는 ESP-IDF 컴포넌트다.

이 컴포넌트는 CC1101 하드웨어를 직접 제어하지 않는다. 무선 패킷 송수신은 `rf_transport`, 최상위 동작 모드 전환은 `main/fsm`, 실제 OTA 플래시 기록과 롤백 처리는 `ota_client`가 담당한다.

## 시스템 내 위치

```text
CC1101
  ↓
cc1101             SPI, 레지스터, FIFO, GDO 인터럽트
  ↓
rf_transport       음성/제어/OTA 패킷 분류 및 송수신
  ↓
ota_client         OTA 세션, 청크 검증, esp_ota, ACK/NACK
  ↓
ESP-IDF bootloader ota_0/ota_1 선택 및 롤백
```

단말은 CC1101 하나로 음성 FHSS와 OTA를 모두 처리한다. `OTA_RECEIVING` 상태에서는 음성 통신을 중단하고 OTA 채널로 이동하므로, 두 경로가 동시에 CC1101을 사용하지 않도록 최상위 FSM이 무선 사용권을 관리해야 한다.

## 담당 범위

- OTA 세션 시작, 진행, 완료, 중단 상태 관리
- 패킷 type, 대상 단말, 세션 ID 검증
- 청크 sequence, 길이, CRC 검증
- 중복 청크 ACK 및 누락 청크 NACK
- `esp_ota_begin()`, `esp_ota_write()`, `esp_ota_end()` 호출
- 이미지 크기와 전체 SHA-256 검증
- `esp_ota_set_boot_partition()`을 통한 다음 부팅 슬롯 선택
- OTA 수신 타임아웃 및 오류 복구
- 새 펌웨어 부팅 성공 확정 또는 이전 이미지 롤백
- 진행률과 성공/실패 이벤트를 최상위 FSM/UI에 전달

## 담당하지 않는 범위

- CC1101 SPI, 레지스터, FIFO, GDO 제어
- FHSS 채널 계산, 호핑, 동기 획득 및 유지
- 음성 프레임 처리
- OLED 직접 제어
- 최상위 애플리케이션 상태 전이
- OTA 패킷 구조체 중복 정의
- 전체 펌웨어 이미지를 RAM에 저장하는 동작

OTA 패킷 형식은 별도 `ota-protocol` 저장소의 공용 헤더를 사용한다. ESP32와 게이트웨이에서 패킷 구조를 각각 다시 정의하지 않는다.

## 권장 파일 구조

```text
components/ota_client/
├── CMakeLists.txt
├── README.md
├── include/
│   └── ota_client.h       외부 공개 API
├── ota_client.c           세션 및 패킷 처리, 전용 태스크/큐
├── ota_writer.c           ESP-IDF OTA 파티션 기록과 검증
└── ota_writer.h           컴포넌트 내부 API
```

초기 구현은 위 구조로 시작하고, 재전송 정책이나 세션 관리가 커질 때만 파일을 추가한다.

## 내부 상태

최상위 애플리케이션 FSM과 별도로 OTA 세션 내부 상태를 관리한다.

```c
typedef enum {
    OTA_CLIENT_IDLE,
    OTA_CLIENT_RECEIVING,
    OTA_CLIENT_VERIFYING,
    OTA_CLIENT_READY_TO_REBOOT,
    OTA_CLIENT_ERROR,
} ota_client_state_t;
```

세션은 최소한 다음 정보를 보관해야 한다.

```c
typedef struct {
    ota_client_state_t state;

    uint32_t session_id;
    uint32_t image_size;
    uint32_t received_bytes;
    uint32_t total_chunks;
    uint32_t expected_sequence;

    uint8_t expected_sha256[32];

    const esp_partition_t *update_partition;
    esp_ota_handle_t ota_handle;

    TickType_t last_packet_tick;
    bool ota_started;
} ota_session_t;
```

## 공개 API 초안

공개 API에는 CC1101 전용 타입을 노출하지 않는다. 이를 통해 USB 기반 모의 전송과 실제 CC1101 전송에서 같은 `ota_client`를 사용할 수 있다.

```c
typedef enum {
    OTA_CLIENT_EVENT_STARTED,
    OTA_CLIENT_EVENT_PROGRESS,
    OTA_CLIENT_EVENT_COMPLETED,
    OTA_CLIENT_EVENT_FAILED,
    OTA_CLIENT_EVENT_ABORTED,
} ota_client_event_t;

typedef esp_err_t (*ota_client_send_callback_t)(
    const uint8_t *packet,
    size_t packet_length,
    void *context
);

typedef void (*ota_client_event_callback_t)(
    ota_client_event_t event,
    uint32_t progress_percent,
    void *context
);

typedef struct {
    uint32_t device_id;
    uint32_t receive_timeout_ms;
    ota_client_send_callback_t send_callback;
    ota_client_event_callback_t event_callback;
    void *callback_context;
} ota_client_config_t;

esp_err_t ota_client_init(const ota_client_config_t *config);

esp_err_t ota_client_submit_packet(
    const uint8_t *packet,
    size_t packet_length
);

esp_err_t ota_client_abort(void);

esp_err_t ota_client_confirm_running_image(void);
```

## 실행 모델

CC1101 수신 태스크 안에서 `esp_ota_write()`를 직접 호출하지 않는다. 플래시 쓰기는 비교적 오래 걸릴 수 있으므로, 수신 데이터는 OTA 전용 큐로 복사하고 `ota_client` 태스크가 처리한다.

```text
GDO 인터럽트
  → cc1101/rf_transport 수신 태스크
  → OTA 패킷 분류
  → ota_client_submit_packet()
  → OTA 전용 FreeRTOS 큐
  → ota_client 태스크
  → 검증 및 esp_ota_write()
  → rf_transport 콜백으로 ACK/NACK 전송
```

최상위 FSM 큐에는 모든 OTA 청크를 전달하지 않는다. 최상위 모드가 바뀌는 시작, 완료, 실패 이벤트만 전달한다.

## 패킷 처리

### OTA_DISCOVER (스캔 응답, 2026-08-12 구조 선반영)

`OTA_START` 이전 단계 — Qt 앱이 OTA 대기 중인 기기를 찾으려고 방송하는 스캔 신호에 대한 응답. `include/ota_discover_packet.h`/`source/ota_discover_packet.c`로 인코드/디코드만 정의(값은 미확정, TODO).

- `OTA_DISCOVER`(Qt 앱 → ESP, 1바이트): `type=6`
- `OTA_DISCOVER_ACK`(ESP → Qt 앱, 7바이트): `type=7` + `device_id` 3바이트 Little Endian + `firmware_version` 3바이트(major/minor/patch)

`main/fsm.c`의 `fsm_post_ota_discover_frame()`이 디코드해 `FSM_EVENT_OTA_DISCOVER_RX`를 올리고, `MENU_OTA` 상태일 때만 `handle_ota_discover_ack()`가 ACK를 인코딩까지 해둔다(상태 전이 없음). 실제 RF 송수신은 `rf_transport`가 없어 TODO — 자세한 배경은 [docs/fsm-design.md](../../docs/fsm-design.md) 결정 이력(2026-08-12) 참고.

### OTA_START

1. 패킷 type을 확인한다.
2. 대상 device ID 또는 브로드캐스트 여부를 확인한다.
3. 새 session ID인지 확인한다.
4. 이미지 크기가 업데이트 파티션보다 작거나 같은지 확인한다.
5. `esp_ota_get_next_update_partition()`으로 대상 슬롯을 선택한다.
6. `esp_ota_begin()`을 호출한다.
7. 세션 정보를 초기화하고 START ACK를 보낸다.

### OTA_DATA

초기 구현은 Selective-Repeat 고정 배치 방식으로 구성한다. RF 패킷 전체 한도가
60바이트이고 `ota-protocol` v0.2 DATA 헤더가 12바이트이므로 DATA payload는
최대 48바이트다. 초기 배치 크기는 5다. 공통 `version` 필드는 사용하지 않고,
OTA 세션 패킷은 `session_id`로 식별한다.

```text
DATA 5개 수신
  → 각 payload CRC와 sequence 범위 검증
  → 배치 RAM cache에 저장
  → 각 DATA sequence에 개별 ACK/NACK

5개가 모두 모임
  → 별도 BATCH_CHECK 없이 sequence 순서로 esp_ota_write()
  → 다음 배치로 이동
```

ESP32의 `received_mask`는 현재 배치가 완성됐는지 판단하는 내부 상태일 뿐 wire에
실리지 않는다. Gateway는 5개를 보낸 뒤 개별 ACK를 받지 못한 sequence만 다시
보낸다. ACK 유실로 이미 처리한 DATA가 다시 들어오면 Flash에 중복 기록하지 않고
해당 sequence ACK를 다시 보낼 수 있도록 성공으로 처리한다. 별도 `BATCH_CHECK`,
`BATCH_ACK`, missing bitmap 패킷은 사용하지 않는다.

### OTA_END

1. 수신한 바이트 수와 이미지 크기가 일치하는지 확인한다.
2. 수신한 청크 수가 일치하는지 확인한다.
3. `esp_ota_end()`로 ESP 이미지 형식과 내부 체크섬을 검증한다.
4. 파티션 전체 SHA-256을 OTA_START에서 받은 값과 비교한다.
5. `esp_ota_set_boot_partition()`을 호출한다.
6. 완료 이벤트를 최상위 FSM에 전달한다.
7. ACK 전송이 끝난 뒤 재부팅한다.

## 스트리밍 기록

펌웨어 전체를 RAM에 저장한 뒤 기록하지 않는다. `OTA_RECEIVING` 중 정상 청크를 받는 즉시 비활성 OTA 파티션에 기록한다.

```text
OTA_RECEIVING
  OTA_START → esp_ota_begin()
  OTA_DATA  → esp_ota_write()
  OTA_END   → 수신 완료

OTA_APPLYING
  esp_ota_end()
  SHA-256 검증
  esp_ota_set_boot_partition()
  esp_restart()
```

## 타임아웃과 중단

수신 중 설정된 시간 동안 유효한 패킷이 없으면 세션을 중단한다.

1. 열린 OTA handle에 `esp_ota_abort()`를 호출한다.
2. 세션 정보를 초기화한다.
3. 실패 또는 중단 이벤트를 FSM에 전달한다.
4. CC1101을 OTA 채널에서 음성 FHSS 경로로 복귀시킨다.

중단된 이미지는 부팅 파티션으로 선택하지 않는다.

## 부팅 확정과 롤백

부트로더 롤백 기능이 활성화된 경우 새 이미지의 첫 부팅 상태는 `ESP_OTA_IMG_PENDING_VERIFY`다. 기본 기능 점검을 통과하면 다음 함수를 호출한다.

```c
esp_ota_mark_app_valid_cancel_rollback();
```

초기화 실패 등 새 이미지에 문제가 있으면 다음 함수로 이전 이미지에 복귀한다.

```c
esp_ota_mark_app_invalid_rollback_and_reboot();
```

정상 여부를 확정하기 전에 재부팅되면 부트로더가 이전 이미지로 롤백할 수 있으므로, 부팅 자체 점검과 확정 시점을 명확히 정의해야 한다.

## FSM 연동

`ota_client`는 상태를 직접 변경하지 않고 콜백이나 `fsm_post_event()`를 통해 결과만 전달한다.

```text
EV_OTA_START       → OTA_RECEIVING 진입
EV_OTA_COMPLETE    → OTA_APPLYING 진입
EV_OTA_VERIFY_OK   → 재부팅
EV_OTA_VERIFY_FAIL → 기존 펌웨어 유지 및 음성 모드 복귀
```

OTA 중에는 음성 FHSS 미수신 카운팅을 중지한다. OTA 종료 후 현재 홉 슬롯으로 재합류하고, 재합류 이후의 연속 수신 실패만 동기 상실로 처리한다.

## 구현 순서

1. `ota_writer`에서 파티션 선택, begin/write/end, 부팅 전환 구현
2. USB 또는 테스트 코드로 순차 청크 입력 검증
3. OTA 세션과 sequence/CRC/ACK/NACK 구현
4. `rf_transport` 콜백 연결
5. CC1101 수신 경로 연결
6. 수신 중 전원 차단, 잘못된 CRC, 누락 청크 시험
7. 새 이미지 부팅 확정과 강제 롤백 시험

## 완료 조건

- 정상 이미지가 비활성 OTA 슬롯에 기록된다.
- 중복 청크가 플래시에 중복 기록되지 않는다.
- 누락/손상 청크에 올바른 NACK를 보낸다.
- 잘못된 크기, CRC, SHA-256 이미지가 부팅 대상으로 선택되지 않는다.
- OTA 중 전원이 차단돼도 기존 정상 이미지로 부팅한다.
- 새 이미지가 정상 동작하면 VALID로 확정된다.
- 새 이미지 초기화가 실패하면 이전 이미지로 롤백한다.
