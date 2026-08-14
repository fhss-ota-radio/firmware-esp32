# FHSS 오디오 통합 테스트 브랜치 현황

> 브랜치: `feature/test/fhss-audio-develop-sync-test`  
> 기준: `develop`의 `c571338`  
> 상태: 기능 통합 및 CC1101 원인 분석 중인 테스트 브랜치. 제품용 merge 전 검증이 필요하다.

## 1. 브랜치 목적

최신 `develop`의 전역 FSM과 오디오 컴포넌트를 유지하면서 다음 경로를 연결하고 실기기에서 검증한다.

```text
PCM 160 samples / 20 ms
    → Speex encode
    → Speex frame 2개를 AUDIO RF packet 하나로 패킹
    → FHSS service
    → CC1101
    → 상대 CC1101
    → AUDIO packet 분리
    → frame별 Speex decode
    → Speaker
```

## 2. 구현된 내용

- `fhss_audio_packet`
  - Speex encoded frame 두 개를 하나의 RF payload로 패킹·언패킹
  - sequence, flags, frame count와 frame별 길이 포함
  - CC1101 payload 최대 60 byte 범위 검증
- `fhss_audio_adapter`
  - 오디오 프레임과 `fhss_service` 사이 연결
  - TX session 시작·종료, 전송 큐, RX frame callback 연결
- `fhss_audio_pcm_test`
  - 마이크 없이 8 kHz/mono/s16le/160-sample PCM 패턴 공급
  - 실제 Speex/FHSS/RF 경로를 20 ms cadence로 테스트
- `fhss_service`
  - SYNC와 오디오 data packet 송수신
  - 역할 전환, TX drain, 오류 callback 및 진단 로그 확장
- `rf_transport`
  - GDO0 timestamp, CC1101 상태 진단, register read-back 추가
  - 현재 핀: MOSI 11, SCLK 12, MISO 13, CS 14, GDO0 10
- 최신 `develop` FSM 연결
  - `MENU_COMM → TX_AUDIO`에서 FHSS TX 시작
  - RX frame을 기존 FSM 오디오 큐로 전달
  - 오류 시 기존 `ERROR` 상태 정책 사용

## 3. 다른 담당 파일 변경

테스트 통합을 위해 다음 팀 공용/타 담당 파일에도 변경이 들어 있다. merge 전 담당자 확인이 필요하다.

- `main/main.c`, `main/fsm.c`, `main/CMakeLists.txt`
- `components/audio_io/audio_io_config.h`
- `components/ptt_button/ptt_button.c`
- `components/rotary_encoder/rotary_encoder.c`
- `docs/fsm-design.md`

`ptt_button`과 `rotary_encoder` 변경은 짧은 polling 주기가 0 tick으로 변환되어 watchdog을 유발하지 않도록 최소 1 tick을 보장하는 내용이다.

## 4. CC1101 진단 결과

### 통합 모드 증상

```text
CC1101 PARTNUM/VERSION이 0x00 또는 0xFF
IOCFG2 expected=0x29, actual=0x00
FHSS service initialization failed
FSM ERROR 진입
```

COM6과 COM10에서 같은 현상을 확인했다.

### 분리 진단

FSM, OLED, audio, PTT, rotary, GDO0 ISR을 시작하지 않는 단독 진단 모드를 `main/main.c`에 추가했다.

GPIO11(MOSI)과 GPIO13(MISO)을 직접 연결한 SPI loopback 결과:

```text
LOOPBACK PASS: status=ESP_OK
TX=A5 5A 3C C3
RX=A5 5A 3C C3
```

따라서 ESP32-S3의 SPI2, GPIO11 출력, GPIO13 입력 및 기본 SPI transaction은 정상이다.

CC1101을 다시 연결한 단독 검사 결과:

```text
pre-config PARTNUM=0x00 VERSION=0x00
IOCFG2 expected=0x29 actual=0x00
configure/read-back status=3
```

FHSS task 우선순위나 다른 컴포넌트 간섭은 배제됐으며, 남은 확인 대상은 CSN(GPIO14), CC1101 reset/명령 처리 및 모듈 핀 해석이다.

## 5. 현재 실행 모드 주의

현재 `main/main.c`는 다음 매크로가 활성화되어 있다.

```c
#define CC1101_STANDALONE_DIAGNOSTIC 1
```

이 상태에서는 FSM, OLED, 오디오 및 실제 FHSS 서비스가 시작되지 않고 CC1101 단독 진단만 실행된다. 정상 애플리케이션 시험 전 반드시 `0`으로 바꾸거나 임시 진단 블록을 제거해야 한다.

## 6. 다음 디버깅 순서

1. CC1101을 분리하고 GPIO14 출력과 GPIO13 입력을 연결해 CS physical loopback 확인
2. 과거 성공한 `57a2be4`의 CC1101 초기화·read 코드를 현재 환경에서 그대로 재현
3. 필요 시 로직 애널라이저로 CSN, SCLK, SI, SO 파형 확인
4. 기본 SPI read-back 성공 후 단독 SYNC TX/RX 재검증
5. 진단 모드 비활성화 후 FSM + PCM source + FHSS 오디오 전체 경로 재검증

## 7. 현재 검증

- `ninja -C build`: 성공
- SPI MOSI/MISO loopback: 성공
- CC1101 register read-back: 실패, 조사 중
- 실제 양방향 음성 통화: 미완료
- PR 및 develop merge: 수행하지 않음

