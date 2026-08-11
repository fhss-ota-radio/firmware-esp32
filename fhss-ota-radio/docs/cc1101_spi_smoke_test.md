# ESP32-S3 ↔ CC1101 SPI Smoke Test

## 1. 테스트 목적

이 테스트는 FHSS 동작이나 실제 RF 패킷 송수신을 검증하기 위한 테스트가 아니다.

검증 범위는 다음 한 경로로 제한한다.

```text
ESP32-S3
  → SPI 명령 전송
  → CC1101 상태 레지스터 접근
  → PARTNUM / VERSION 읽기
  → UART Monitor에 결과 출력
```

CC1101이 올바른 PARTNUM과 VERSION을 반환하면 다음 항목이 기본적으로 정상이라고 판단할 수 있다.

- CC1101 전원 공급
- ESP32-S3와 CC1101의 공통 GND
- SCLK, MOSI, MISO, CS 배선
- SPI Mode 0 통신
- CC1101 CS 및 SO(MISO) 준비 신호 처리
- CC1101 상태 레지스터 읽기

이 테스트만으로 안테나, 송신 출력, 수신 감도, 주파수 설정, FHSS 또는 OTA 통신이 정상이라고 판단할 수는 없다.

## 2. 테스트 환경

| 항목 | 값 |
|---|---|
| MCU | ESP32-S3 |
| RF IC | CC1101 |
| ESP-IDF Target | `esp32s3` |
| SPI Host | `SPI2_HOST` |
| SPI Mode | Mode 0 |
| SPI Clock | 1 MHz |
| Monitor Baud | 115200 |

### 테스트 코드의 GPIO 설정

| CC1101 신호 | ESP32-S3 GPIO | 방향 |
|---|---:|---|
| SCLK | GPIO12 | ESP32-S3 → CC1101 |
| MOSI / SI | GPIO11 | ESP32-S3 → CC1101 |
| MISO / SO | GPIO13 | CC1101 → ESP32-S3 |
| CS / CSn | GPIO14 | ESP32-S3 → CC1101 |
| VCC | 3.3V | 전원 |
| GND | GND | 공통 접지 |

> 실제 배선이 다르면 `main/main.c` 상단의 `CC1101_SMOKE_*_GPIO` 매크로를 실제 GPIO에 맞게 변경해야 한다. CC1101에는 5V가 아닌 3.3V를 사용한다.

## 3. 변경된 파일

| 파일 | 변경 목적 |
|---|---|
| `main/main.c` | 임시 Smoke Test 함수와 부팅 시 1회 호출 추가 |
| `main/CMakeLists.txt` | `main`에서 `rf_transport` 공개 API를 사용할 수 있도록 의존성 추가 |
| `components/rf_transport/rf_transport.c` | SPI 버스 초기화와 CC1101 PARTNUM/VERSION 읽기 구현 |
| `components/rf_transport/include/rf_transport.h` | 설정, 상태, 칩 정보 구조체와 공개 API 정의 |
| `components/rf_transport/CMakeLists.txt` | SPI, GPIO, 타이머 컴포넌트 의존성 등록 |

## 4. `main/main.c` 테스트 코드

테스트 함수는 요구사항에 따라 별도 테스트 파일이 아니라 기존 `main/main.c`에 작성했다.

```c
static void cc1101_smoke_test(void)
{
    static rf_transport_t transport;

    const rf_transport_config_t config = {
        .spi_host = SPI2_HOST,
        .sclk_gpio = GPIO_NUM_12,
        .mosi_gpio = GPIO_NUM_11,
        .miso_gpio = GPIO_NUM_13,
        .cs_gpio = GPIO_NUM_14,
        .spi_clock_hz = 1000000,
    };

    const rf_transport_status_t init_status =
        rf_transport_init(&transport, &config);
    if (init_status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "rf_transport_init failed: status=%d", init_status);
        return;
    }

    rf_transport_chip_info_t chip_info = {0};
    const rf_transport_status_t read_status =
        rf_transport_read_chip_info(&transport, &chip_info);
    if (read_status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "CC1101 chip-info read failed: status=%d", read_status);
        return;
    }

    ESP_LOGI(TAG,
             "CC1101 SPI OK: PARTNUM=0x%02X, VERSION=0x%02X",
             chip_info.partnum,
             chip_info.version);
}
```

`app_main()`에서는 이 함수를 한 번 호출한 뒤 기존 FSM 초기화를 그대로 계속한다.

```c
void app_main(void)
{
    cc1101_smoke_test();

    fsm_init();
    fsm_post_event(FSM_EVENT_INIT_DONE);
}
```

따라서 Smoke Test 실패가 발생해도 기존 애플리케이션은 계속 실행된다.

## 5. 코드 동작 설명

### 5.1 설정 생성

`rf_transport_config_t`에 SPI Host, GPIO와 SPI Clock을 담는다. GPIO를 `rf_transport` 내부에 고정하지 않기 때문에 실제 보드 배선이 변경되어도 호출부 설정만 변경할 수 있다.

### 5.2 SPI 버스 초기화

`rf_transport_init()`은 다음 순서로 동작한다.

1. 포인터, GPIO, SPI Clock 값 검사
2. `spi_bus_initialize()`로 SPI2 버스 초기화
3. CS GPIO를 수동 출력으로 설정하고 HIGH로 비활성화
4. SPI Mode 0, 1 MHz 장치 등록
5. 초기화된 SPI handle과 GPIO 정보를 `rf_transport_t`에 저장

CS를 ESP-IDF SPI 드라이버의 자동 CS에 맡기지 않고 수동으로 제어한다. CC1101은 CS를 LOW로 만든 뒤 SO(MISO)가 LOW가 될 때까지 기다려야 하기 때문이다.

### 5.3 CC1101 준비 상태 대기

트랜잭션 시작 전 다음 순서로 처리한다.

```text
CS LOW
  → SO(MISO)가 LOW가 될 때까지 대기
  → SPI transaction
  → CS HIGH
```

SO가 10ms 안에 LOW가 되지 않으면 `RF_TRANSPORT_STATUS_TIMEOUT`을 반환한다. 이 경우 전원, GND, CS 또는 MISO 배선을 우선 확인해야 한다.

### 5.4 PARTNUM과 VERSION 읽기

CC1101 상태 레지스터 주소는 다음과 같다.

| 레지스터 | 주소 | 의미 |
|---|---:|---|
| PARTNUM | `0x30` | 칩 부품 번호 |
| VERSION | `0x31` | 칩 버전 |

CC1101 상태 레지스터 읽기에는 Read Bit와 Burst Bit가 모두 포함된 명령 바이트를 사용한다.

```text
command = 0xC0 | register_address
```

전송 예시는 다음과 같다.

```text
PARTNUM: TX [0xF0, 0x00] → RX 두 번째 바이트가 PARTNUM
VERSION: TX [0xF1, 0x00] → RX 두 번째 바이트가 VERSION
```

두 레지스터를 모두 성공적으로 읽은 뒤에만 호출자의 출력 구조체에 결과를 복사한다.

## 6. 상태 코드 해석

| 값 | 상태 | 의미 |
|---:|---|---|
| 0 | `RF_TRANSPORT_STATUS_OK` | 정상 처리 |
| 1 | `RF_TRANSPORT_STATUS_INVALID_ARG` | 포인터, GPIO 또는 Clock 설정 오류 |
| 2 | `RF_TRANSPORT_STATUS_NOT_INITIALIZED` | 초기화 전에 레지스터 읽기 호출 |
| 3 | `RF_TRANSPORT_STATUS_SPI_ERROR` | SPI 버스 또는 transaction 오류 |
| 4 | `RF_TRANSPORT_STATUS_TIMEOUT` | CS LOW 후 SO(MISO)가 준비되지 않음 |

## 7. 빌드 및 Flash 결과

### 빌드

전체 ESP-IDF 프로젝트 빌드가 성공했다.

```text
Building C object ... rf_transport.c.obj
Building C object ... main.c.obj
Linking C static library ... librf_transport.a
Linking CXX executable fhss-ota-radio.elf
Successfully created ESP32-S3 image.
```

생성된 애플리케이션 정보:

| 항목 | 결과 |
|---|---|
| 바이너리 | `build/fhss-ota-radio.bin` |
| 크기 | `0x4d630` |
| 최소 앱 파티션 여유 | 70% |

### Flash

COM3에서 ESP32-S3 연결, 이미지 기록과 해시 검증이 모두 성공했다.

```text
Connected to ESP32-S3 on COM3
Writing 'bootloader/bootloader.bin' ...
Hash of data verified.
Writing 'partition_table/partition-table.bin' ...
Hash of data verified.
Writing 'fhss-ota-radio.bin' ...
Hash of data verified.
Hard resetting via RTS pin...
```

## 8. 실제 Smoke Test 결과

실제 Monitor 로그에서 다음 결과를 확인했다.

```text
I (287) cc1101_smoke: Starting CC1101 SPI smoke test (SCLK=12, MOSI=11, MISO=13, CS=14)
I (297) cc1101_smoke: CC1101 SPI OK: PARTNUM=0x00, VERSION=0x14
```

### 판정

| 검증 항목 | 결과 |
|---|---|
| ESP32-S3 SPI 초기화 | PASS |
| CC1101 SO 준비 신호 | PASS |
| PARTNUM 읽기 | PASS (`0x00`) |
| VERSION 읽기 | PASS (`0x14`) |
| ESP32-S3 ↔ CC1101 기본 SPI 통신 | **PASS** |

이번 1차 Smoke Test의 목표는 달성됐다.

## 9. 함께 출력된 오류와 테스트 결과의 관계

### OLED 오류

```text
E (...) display_ui: ssd1306 init sequence failed
E (...) display_ui: update row 0 failed
```

이 오류는 기존 FSM이 OLED를 초기화하는 과정에서 발생한 것으로 CC1101 SPI 결과와 무관하다.

### Task Watchdog

```text
E (...) task_wdt: Task watchdog got triggered.
E (...) task_wdt:  - IDLE0 (CPU 0)
E (...) task_wdt: CPU 0: ptt_button
E (...) task_wdt: CPU 1: rotary_encoder
```

백트레이스가 `ptt_button_task()`의 `vTaskDelay()`를 가리키더라도 CC1101 오류를 의미하지 않는다.

현재 설정에서는 짧은 폴링 시간이 tick으로 변환될 때 0이 될 수 있다.

```text
PTT:    pdMS_TO_TICKS(5ms) → 0 tick 가능
Rotary: pdMS_TO_TICKS(2ms) → 0 tick 가능
```

이 경우 높은 우선순위의 폴링 태스크가 IDLE 태스크에 실행 시간을 주지 않아 Watchdog가 반복된다. CC1101 로그는 부팅 직후 한 번만 출력되기 때문에 이후 반복되는 Watchdog 로그에 밀려 화면에서 찾기 어려울 수 있다.

## 10. 현재 테스트를 다시 확인하는 방법

1. 먼저 Monitor를 연다.

   ```powershell
   idf.py -p COM3 monitor
   ```

2. Monitor가 연결된 상태에서 ESP32-S3의 `RESET/EN` 버튼을 누른다.
3. 부팅 초반의 `cc1101_smoke` 로그를 확인한다.
4. 종료할 때는 `Ctrl+]`를 누른다.

성공 판단에 필요한 핵심 문자열은 다음 한 줄이다.

```text
CC1101 SPI OK: PARTNUM=0x00, VERSION=0x14
```

## 11. 더 보기 좋은 테스트 방법

### 개선안 1: 명확한 PASS/FAIL 배너 출력

가장 먼저 적용하기 좋은 방법이다. 일반 로그 사이에서도 결과가 눈에 띄도록 여러 줄의 배너를 출력한다.

```text
==================================================
 CC1101 SPI SMOKE TEST: PASS
 PARTNUM : 0x00
 VERSION : 0x14
==================================================
```

실패할 때도 상태 코드와 점검 대상을 함께 출력한다.

```text
==================================================
 CC1101 SPI SMOKE TEST: FAIL
 STATUS  : TIMEOUT (4)
 CHECK   : VCC / GND / CS / MISO wiring
==================================================
```

### 개선안 2: Smoke Test 전용 모드에서 FSM 시작 보류 (적용됨)

Smoke Test 직후 FSM이 시작되면 OLED와 Watchdog 로그가 이어진다. 현재는 임시 매크로로 테스트 모드를 구분해 결과만 깨끗하게 볼 수 있도록 적용했다.

```c
#define CC1101_SMOKE_TEST_ONLY 1

void app_main(void)
{
    cc1101_smoke_test();

#if !CC1101_SMOKE_TEST_ONLY
    fsm_init();
    fsm_post_event(FSM_EVENT_INIT_DONE);
#endif
}
```

장점:

- CC1101 결과만 출력됨
- OLED와 버튼 태스크 오류가 섞이지 않음
- Smoke Test와 전체 애플리케이션 문제를 분리하기 쉬움

주의점:

- 임시 테스트가 끝나면 매크로를 0으로 바꾸거나 코드를 제거해야 한다.
- 제품 코드에 테스트 모드가 계속 활성화되지 않도록 확인해야 한다.

### 개선안 3: 여러 번 읽고 성공률 출력

한 번만 읽는 대신 PARTNUM/VERSION을 10회 읽어 결과가 매번 같은지 확인한다.

```text
CC1101 SPI READ 1/10: PARTNUM=0x00 VERSION=0x14 PASS
...
CC1101 SPI READ 10/10: PARTNUM=0x00 VERSION=0x14 PASS
SUMMARY: 10/10 PASS
```

이 방식은 단순 연결 성공뿐 아니라 접촉 불량, 긴 배선, 불안정한 전원 때문에 발생하는 간헐적 SPI 오류를 찾는 데 유용하다.

### 개선안 4: 기대값 검증

현재 코드는 읽기 성공 여부만 검사한다. 다음 값도 명시적으로 검사하면 잘못된 고정값을 성공으로 오판할 가능성을 줄일 수 있다.

- PARTNUM이 기대값 `0x00`인지 확인
- VERSION이 `0x00` 또는 `0xFF`처럼 비정상적으로 고정되지 않는지 확인
- 동일한 보드와 CC1101 모듈에서 확인한 VERSION 기준값과 비교

VERSION은 CC1101 실리콘 또는 모듈에 따라 다를 수 있으므로 무조건 `0x14`만 허용하기 전에 사용 중인 부품의 기준값을 확정해야 한다.

### 개선안 5: Logic Analyzer 병행

문제가 생길 경우 SCLK, MOSI, MISO, CS 네 신호를 Logic Analyzer로 확인하면 원인을 가장 빠르게 구분할 수 있다.

확인할 항목:

- Idle 상태에서 CS가 HIGH인지
- transaction 동안 CS가 LOW인지
- SPI Clock이 약 1 MHz인지
- Mode 0에 맞게 Clock idle이 LOW인지
- MOSI에 `0xF0`, `0xF1` 명령이 보이는지
- MISO 두 번째 바이트에 `0x00`, `0x14`가 보이는지

## 12. 권장 다음 단계

1. PASS/FAIL 배너 적용
2. 임시 Smoke Test 전용 모드로 FSM 로그 분리
3. PARTNUM/VERSION 10회 반복 읽기 및 성공률 확인
4. PTT/Rotary의 0 tick 지연 문제 수정
5. CC1101 reset strobe와 기본 레지스터 설정 테스트
6. 송수신 테스트는 그 이후 별도 단계로 진행

## 13. 최종 결론

ESP32-S3에서 SPI를 통해 CC1101의 PARTNUM `0x00`과 VERSION `0x14`를 읽었다. 따라서 이번 테스트 범위인 ESP32-S3와 CC1101 사이의 기본 SPI 연결은 정상이다.

현재 화면에 반복되는 Task Watchdog와 OLED 오류는 별개의 기존 애플리케이션 문제이며, CC1101 SPI Smoke Test의 PASS 판정을 변경하지 않는다.
