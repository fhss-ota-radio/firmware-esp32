# FHSS Service와 Main FSM 연동

## 1. 목적

프로젝트에는 서로 책임이 다른 두 상태기계가 있다. 이 문서는 둘을 하나로 합치지 않고 어떤 이벤트와 데이터만 경계를 통과하는지 정의한다.

```text
사용자·제품 모드                    무선 동기 상태
main/fsm                           fhss_service/fhss_fsm
─────────────────                 ─────────────────────
MENU_COMM                         STOPPED
TX_AUDIO          ── start TX ──> TRANSMITTING
RX_AUDIO          <─ audio data ─ SEARCHING
                                   SYNCHRONIZING
                                   TRACKING
MENU_COMM         <─ SYNC_LOST ── SEARCHING
```

## 2. 책임 분리

| 계층 | 담당하는 것 | 담당하지 않는 것 |
|---|---|---|
| `main/fsm` | 메뉴, PTT, 송수신 세션, OTA, 오디오 태스크 | 홉 인덱스, slot 계산, timing window |
| `fhss_service` | CC1101 운용, SYNC 송수신, slot 시각, 채널 변경 | OLED 메뉴, PTT UX, 오디오 decode |
| `fhss_fsm` | SEARCHING/SYNCHRONIZING/TRACKING/TRANSMITTING | 제품 MENU/TX_AUDIO/RX_AUDIO 상태 |
| `fhss_audio_packet` | Speex frame 1~2개의 RF payload 변환 | codec 연산, RF 송수신, FSM 전이 |

## 3. 상태 대응

두 FSM 상태는 1:1 대응하지 않는다.

| Main FSM | 허용되는 FHSS 동작 |
|---|---|
| `BOOT_INIT` | 라디오 초기화 준비 |
| `MENU_COMM` | 시작 채널에서 음성 세션 대기 |
| `MENU_IDLE` | 음성 수신·송신 정지 |
| `MENU_OTA` | OTA 시작 packet 대기 |
| `TX_AUDIO` | `TRANSMITTING`, audio packet 송신 |
| `RX_AUDIO` | `SYNCHRONIZING` 또는 `TRACKING`, audio packet 수신 |
| `OTA_RECEIVING`/`OTA_APPLYING` | 음성 FHSS miss 카운팅 정지 |

`SYNC_ACQUIRED`는 FHSS 내부 관측 이벤트다. 제품 모드를 바꾸지 않으므로 Main FSM 이벤트로 전달하지 않는다.

`SYNC_LOST`는 완전히 호핑 추종을 놓친 이상 상황이다. adapter가 다음처럼 변환한다.

```text
FHSS_SERVICE_EVENT_SYNC_LOST
              ↓
fsm_post_event(FSM_EVENT_SYNC_LOST)
              ↓
MENU_COMM으로 안전 복귀
```

## 4. TX 오디오 경로

목표 데이터 흐름:

```text
PTT_PRESS
  ↓
TX_AUDIO
  ↓ audio_io_capture_encode(), 20 ms Speex frame
2-frame aggregator
  ↓ 40 ms마다 최대 49-byte audio packet
FHSS slot/channel scheduler
  ↓
rf_transport_send_packet()
```

현재 `audio_codec`은 quality=4 CBR에서 frame당 20 bytes를 만들며 두 frame packet은 49 bytes다. PTT가 frame 하나만 남긴 채 해제되면 `END_OF_TALKSPURT` flag로 1-frame packet을 flush한다.

## 5. RX 오디오 경로

목표 데이터 흐름:

```text
CC1101 GDO0 + FIFO packet
  ↓ CRC/type 검증
fhss_audio_packet_unpack()
  ↓ frame 0
fsm_post_rx_audio_frame()
  ↓ frame 1
fsm_post_rx_audio_frame()
  ↓
rx_audio_task → audio_io_decode_play()
```

`fsm_post_rx_audio_frame()`은 데이터를 Main FSM의 4-depth 큐로 복사한다. unpack의 zero-copy frame view는 두 호출이 끝난 뒤 폐기할 수 있다.

첫 `RX_FRAME`은 `MENU_COMM → RX_AUDIO` 전이를 만든다. 같은 packet의 두 번째 frame과 이후 frame은 `RX_AUDIO → RX_AUDIO` 상태 유지 전이로 처리한다. `fsm_transition_to()`가 동일 상태에서 즉시 반환하므로 재생 태스크는 재생성되지 않는다.

## 6. Packet loss

- audio packet sequence gap 하나는 기본적으로 Speex 20 ms frame 두 개, 즉 40 ms 손실이다.
- 현재 정책은 손실 frame skip이며 잘못된 payload를 decoder에 전달하지 않는다.
- Main FSM의 1초 idle timeout은 packet loss 한두 번으로 수신 세션을 끝내지 않는다.
- PLC 또는 silence 삽입은 `audio_codec`의 공식 API가 추가된 후 적용한다.

## 7. 동시성과 큐

- RF 수신 태스크는 packet 검증과 unpack만 하고 frame을 Main FSM 큐로 복사한 뒤 반환한다.
- Speex decode와 스피커 출력은 `rx_audio_task`에서 수행한다.
- 한 RF packet이 큐 두 칸을 한 번에 사용한다. 현재 깊이 4는 20 ms frame 네 개, 약 80 ms다.
- 큐가 가득 차면 `fsm_post_rx_audio_frame()`이 `false`를 반환하므로 drop 통계를 증가시켜야 한다.
- ISR에서는 codec, packet unpack, FSM API를 직접 호출하지 않는다. GDO0 ISR은 timestamp 전달만 담당한다.

## 8. 현재 구현과 남은 작업

완료:

- 두 FSM의 독립 구현
- `FHSS_SERVICE_EVENT_SYNC_LOST` callback 정의
- 오디오 frame 전용 Main FSM 큐
- 2-frame packet pack/unpack
- RX 연속 frame 상태 유지 전이

남은 작업:

1. 제품용 FHSS service 생성·초기화 adapter
2. PTT 상태에 따른 TX/RX role 동적 전환
3. TX 2-frame aggregator
4. RX SYNC/audio packet type 분기
5. `SYNC_LOST` callback의 Main FSM 연결
6. OTA 진입/종료 시 FHSS pause/resume
7. end-to-end 음성 지연, queue drop, packet loss 측정

현재 `examples/fhss_sync_test`는 고정 TX/RX 역할의 하드웨어 시험용이다. 제품 `main`에 그대로 복사하지 않고 위 adapter가 동적 세션 역할을 제어해야 한다.
