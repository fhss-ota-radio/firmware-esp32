# Speex 2-Frame FHSS 오디오 패킷화

## 1. 확인된 오디오 계약

최신 `audio_codec` 구현 기준 설정은 다음과 같다.

| 항목 | 값 |
|---|---|
| PCM | signed 16-bit PCM (`int16_t`) |
| 채널 | mono |
| sample rate | 8 kHz |
| frame samples | 160 samples |
| frame duration | 20 ms |
| Speex mode | narrowband |
| quality | 4 |
| VBR | 미사용(CBR) |
| 현재 encoded 크기 | 20 bytes |
| 호출자 권장 버퍼 | `AUDIO_CODEC_MAX_ENCODED_BYTES` = 64 bytes |

현재 quality=4 CBR 설정에서는 한 프레임이 20 bytes지만 프로토콜이 20 bytes를 보장하는 것은 아니다. quality 또는 encoder 설정이 바뀌면 크기가 달라질 수 있으므로 실제 반환 길이를 패킷에 기록한다.

`audio_codec_encode()`와 `audio_codec_decode()`는 호출자 스레드에서 연산을 마칠 때까지 반환하지 않는 blocking 함수다. 실행 시간은 아직 실측되지 않았다.

## 2. 두 프레임을 묶을 수 있는가

현재 `rf_transport` payload 한도는 60 bytes다. 새 오디오 헤더는 9 bytes이고 현재 Speex frame 두 개는 40 bytes다.

```text
9-byte header + 20-byte frame 0 + 20-byte frame 1 = 49 bytes
```

따라서 현재 설정에서는 두 프레임, 즉 40 ms 분량을 RF packet 하나에 안전하게 넣을 수 있고 11 bytes가 남는다.

단, 두 encoded frame의 합이 51 bytes를 넘으면 pack은 `FHSS_AUDIO_PACKET_STATUS_PAYLOAD_TOO_LARGE`로 실패한다. 64-byte 최대 버퍼는 frame의 고정 전송 크기가 아니라 codec 호출용 여유 버퍼다.

## 3. 패킷 형식

| Offset | 크기 | 필드 |
|---:|---:|---|
| 0 | 1 | magic `0xA5` |
| 1 | 1 | version `1` |
| 2 | 1 | type `0x02` (audio) |
| 3 | 1 | flags |
| 4 | 1 | frame count (`1` 또는 `2`) |
| 5 | 2 | packet sequence, little-endian |
| 7 | 1 | frame 0 encoded length |
| 8 | 1 | frame 1 encoded length, 없으면 `0` |
| 9 | N | encoded frame data |

`END_OF_TALKSPURT` flag를 사용하면 PTT가 해제될 때 frame 하나만 남은 마지막 packet도 전송할 수 있다.

## 4. 송신 사용 흐름

```c
uint8_t encoded[2][AUDIO_CODEC_MAX_ENCODED_BYTES];
int encoded_len[2];

encoded_len[0] = audio_codec_encode(pcm0, encoded[0], sizeof(encoded[0]));
encoded_len[1] = audio_codec_encode(pcm1, encoded[1], sizeof(encoded[1]));

fhss_audio_frame_view_t frames[2] = {
    { .data = encoded[0], .length = (size_t)encoded_len[0] },
    { .data = encoded[1], .length = (size_t)encoded_len[1] },
};

uint8_t rf_payload[RF_TRANSPORT_MAX_PACKET_LENGTH];
size_t rf_length = 0;
fhss_audio_packet_pack(sequence, 0, frames, 2,
                       rf_payload, sizeof(rf_payload), &rf_length);
```

모든 버퍼는 호출자가 생성하고 소유한다. pack 함수는 입력 frame을 출력 packet으로 복사한다.

## 5. 수신 및 FSM 연결

unpack 결과의 frame 포인터는 수신 RF buffer 내부를 가리키는 zero-copy view다. RF buffer가 유효한 동안 즉시 frame별로 처리해야 한다.

```c
fhss_audio_packet_view_t packet;
if (fhss_audio_packet_unpack(rf_payload, rf_length, &packet) ==
    FHSS_AUDIO_PACKET_STATUS_OK) {
    for (size_t i = 0; i < packet.frame_count; ++i) {
        fsm_post_rx_audio_frame(packet.frames[i].data,
                                packet.frames[i].length);
    }
}
```

팀 `main/fsm`의 큐는 호출 시 데이터를 내부 버퍼로 복사하므로 위 호출이 반환된 뒤 RF buffer를 재사용할 수 있다. RF packet 하나에 frame 두 개가 있어도 decoder는 반드시 frame별로 한 번씩, 총 두 번 호출된다.

현재 `fsm_post_rx_audio_frame()`은 frame을 enqueue할 때마다 `FSM_EVENT_RX_FRAME`도 발생시킨다. 두 frame을 연속 전달하면 첫 이벤트가 `MENU_COMM`에서 `RX_AUDIO`로 전이시키고, 두 번째 이벤트는 이미 실행 중인 수신 세션의 다음 frame을 뜻한다.

담당자 승인에 따라 `RX_AUDIO + RX_FRAME -> RX_AUDIO` 상태 유지 전이를 `main/fsm.c`에 추가했다. `fsm_transition_to()`는 동일 상태 요청이면 즉시 반환하므로 재생 태스크를 재생성하지 않으며, 두 번째 이벤트를 unhandled 경고 없이 소비한다. 이 브랜치에서 수정한 팀 소유 파일은 이 전이와 설명 주석을 추가한 `main/fsm.c` 한 곳뿐이다.

## 6. Packet loss 정책

현재 `audio_codec_decode()`에는 PLC 전용 API가 노출되어 있지 않고 팀 정책도 손실 frame skip이다.

- packet sequence가 하나 건너뛰면 40 ms 오디오가 손실된 것으로 기록한다.
- 손실 packet을 decoder에 잘못된 데이터로 전달하지 않는다.
- 현재는 silence 삽입이나 PLC 호출 없이 다음 정상 frame부터 decode한다.
- 추후 PLC를 적용하려면 `audio_codec` 팀과 `speex_decode_int(..., NULL, ...)`를 감싼 공식 API 계약을 먼저 추가해야 한다.

두 frame을 묶으면 RF overhead는 줄지만 packet 하나 손실 시 20 ms가 아니라 40 ms가 동시에 사라지는 trade-off가 있다.

## 7. 다음 연동 단계

1. TX capture callback에서 frame 두 개를 모으는 aggregator 추가
2. PTT release 시 남은 한 frame을 `END_OF_TALKSPURT`로 flush
3. FHSS synchronized slot에서 audio packet 송신
4. RX에서 SYNC/audio packet type 분기
5. audio packet을 unpack하고 frame별 `fsm_post_rx_audio_frame()` 호출
6. encode/decode 실행 시간과 packet loss 통계 측정

4~5번에서 `main/fsm` 연결이 필요하지만 팀 파일을 직접 수정하지 않고 FHSS adapter callback을 우선 제공한다.
