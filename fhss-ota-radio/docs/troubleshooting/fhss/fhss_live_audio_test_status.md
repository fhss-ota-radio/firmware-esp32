# FHSS 실시간 음성 통합 테스트 현황

## 목적

두 ESP32-S3/CC1101 보드에서 다음 실제 음성 경로를 검증한다.

```text
PTT -> INMP441 -> Speex encode -> FHSS/CC1101
    -> CC1101/FHSS -> Speex decode -> MAX98357A
```

## 현재 브랜치와 주요 변경

- 브랜치: `test/fhss-live-audio-call`
- `10f8ac5`: CC1101/FHSS 통합 동기화 안정화
- `fd645d2`: 한 슬롯 안에서 SYNC 이후 AUDIO DATA 패킷까지 수신하도록 개선
- `c06f239`: 합성 PCM 입력을 끄고 실제 마이크 캡처 경로 활성화

팀원 소유인 `components/audio_io/audio_io_config.h`의 로컬 변경은 이 작업의
커밋에 포함하지 않는다.

## 실기기 환경

- COM6: 송신 보드
- COM10: 수신 보드
- CC1101: 433 MHz
- CC1101 SPI: SCLK GPIO12, MOSI GPIO9, MISO GPIO11, CS GPIO10
- CC1101 GDO0: GPIO13
- PTT: GPIO1, active-low
- 마이크: WS GPIO4, BCLK GPIO5, SD GPIO6

## 검증 완료 범위

다음 경로는 두 보드의 로그로 확인했다.

1. PTT 입력과 `MENU_COMM -> TX_AUDIO` 전이
2. FHSS TX 세션 시작 및 채널 호핑
3. SYNC 패킷 수신과 RX의 `TRACKING` 유지
4. Speex frame 두 개를 담은 AUDIO RF packet 송신
5. 상대 보드의 AUDIO RF packet 수신
6. RF packet을 Speex frame으로 분리
7. 수신 frame을 FSM 오디오 큐와 `rx_audio` 태스크에 전달
8. PTT 해제 후 `TX_AUDIO -> MENU_COMM` 복귀

대표 로그:

```text
[COM6] fhss_audio_adapter: AUDIO_TX packet=175 sequence=174 frames=2 bytes=49
[COM10] fhss_service: SYNC RX: state=TRACKING slot=21 channel=0
[COM10] fhss_audio_adapter: AUDIO_RX packet=175 sequence=195 frames=2 bytes=49
[COM6] fsm: TX_AUDIO -> MENU_COMM
[COM10] fsm: RX_AUDIO -> MENU_COMM
```

따라서 FHSS 동기화, CC1101 송수신 및 오디오 패킷 운반 경로는 동작한다.

## 현재 실패 지점

실제 음성은 스피커에서 들리지 않았다. RF 경로와 분리하기 위해 COM6을
`MENU_IDLE`로 설정하고 로컬 loopback을 실행한 결과 마이크 I2S 읽기가 실패했다.

```text
fsm: MENU_COMM -> MENU_IDLE
ptt_button: pressed
audio_io: mic read failed (err=259, bytes=0)
ptt_button: released
fsm: mic test: 0 frames captured, peak=0/32767, playback in 1s
```

`259`는 이 ESP-IDF 환경에서 `ESP_ERR_INVALID_STATE`다. PTT 해제 시
`i2s_channel_read()` 안에서 대기 중인 TX 태스크를 외부에서 강제 삭제하면서
드라이버의 읽기 상태가 해제되지 않은 것이 원인이었다. 따라서 핀이나 clock
설정 문제가 아니라 I2S 호출 도중의 태스크 수명 관리 문제였다.

## 과거 loopback 성공 코드와 비교

과거 loopback 성공 이력의 `audio_io.c`와 현재 파일에서 마이크 초기화 및 캡처
구현 차이는 발견되지 않았다.

- I2S0 master
- 8 kHz
- 32-bit mono slot
- `I2S_STD_SLOT_RIGHT`
- 부팅 시 RX channel enable
- `i2s_channel_read()`로 160 sample 수집

마이크 GPIO4/5/6도 동일하다. 따라서 핀 번호나 Speex 처리로 원인을 단정하지
않고, FHSS 통합 후 실행 환경과 I2S/GDMA/clock 상태를 우선 분리 진단한다.

## 추가 관찰 사항

FHSS 추종은 유지됐지만 일부 AUDIO sequence가 한 개씩 누락됐다.

```text
audio packet gap: expected=168 received=169
audio packet gap: expected=183 received=184
audio packet gap: expected=198 received=199
```

실제 음성 경로 복구 후 누락률과 체감 끊김을 함께 측정해야 한다.

## 다음 진단 순서

### 1. FHSS 초기화 A/B 테스트

같은 펌웨어에서 FHSS service 초기화만 임시로 끈 빌드와 켠 빌드를 비교한다.

- FHSS OFF에서 loopback 성공: FHSS 통합 이후 자원 또는 초기화 충돌 조사
- FHSS OFF에서도 timeout: audio I2S clock/DMA 상태 조사

### 2. I2S RX 상태 확인

- PTT 직전 RX channel disable/enable 후 read 비교
- GPIO5 BCLK와 GPIO4 WS의 실제 파형 측정
- 첫 `i2s_channel_read()`의 반환값, bytes 및 소요 시간 기록

### 3. 출력 경로 분리

- 고정 PCM tone을 I2S TX로 직접 재생
- 스피커가 정상이면 마이크 캡처만 집중 진단
- tone도 들리지 않으면 MAX98357A, SD/GAIN 및 I2S TX를 별도로 점검

### 4. 전체 통합 재시험

로컬 loopback 성공 후 두 보드를 모두 `COMM`으로 설정한다. COM6의 PTT를 누른
상태에서 말하고 COM10에서 재생, sequence gap, decode 실패 및 큐 overflow를
동시에 확인한다.

## 2026-08-16 재시험 결과

### 마이크 I2S 잠김 수정 확인

팀원 패치 `902a555`는 `vTaskDelete(s_tx_audio_task)`를 협조적 종료 플래그로
교체했다. 실행 중인 I2S read가 반환된 뒤 TX 태스크가 스스로 종료하게 만들었다.

새 펌웨어를 플래시한 뒤 COM10의 `MENU_IDLE` loopback에서 다음 결과를 확인했다.

```text
fsm: MENU_COMM -> MENU_IDLE
ptt_button: pressed
ptt_button: released
fsm: mic test: 168 frames captured, peak=1009/32767, playback in 1s
```

- `err=259` 재발 없음
- 약 3.36초 분량의 실제 마이크 frame 캡처 성공
- Speex encode/decode 및 PCM peak 확인 성공

### 다음 차단점: RX 스피커 I2S 재활성화 실패

COM10을 TX, COM6을 RX로 둔 실제 FHSS 시험에서 AUDIO packet 수신과
`MENU_COMM -> RX_AUDIO` 전이까지 성공했지만, COM6이 스피커를 활성화하는
순간 재부팅됐다.

```text
[COM10] fsm: MENU_COMM -> TX_AUDIO
[COM10] fhss_audio_adapter: TX session started
[COM6] fhss_service: SYNC RX: state=SEARCHING slot=0 channel=0
[COM6] fsm: MENU_COMM -> RX_AUDIO
[COM6] ESP_ERROR_CHECK failed: ESP_ERR_INVALID_STATE
[COM6] components/audio_io/audio_io.c line 122
[COM6] audio_io_speaker_enable(s_spk_tx)
```

`rx_audio_task()`는 timeout 종료 전에 `s_rx_audio_task = NULL`로 만든다. 이후
`on_enter_menu_comm()`은 `s_rx_audio_task != NULL`일 때만 speaker disable을
호출하므로 I2S TX channel이 활성 상태로 남을 수 있다. 다음 `RX_AUDIO` 진입에서
이미 활성화된 channel을 다시 enable하면서 `ESP_ERR_INVALID_STATE`가 발생한다.

다음 수정은 RX 태스크에도 협조적 종료를 적용하고, 정상 종료·SYNC_LOST·ERROR
모든 경로에서 speaker channel을 정확히 한 번 disable하도록 만드는 것이다.
