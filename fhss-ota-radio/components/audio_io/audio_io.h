#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_io_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 마이크(I2S_NUM_0)/스피커(I2S_NUM_1) 채널 초기화. app_main 등에서 한 번 호출.
 * audio_codec_init()도 별도로 호출해야 한다 — 이 컴포넌트는 I2S 입출력만 담당.
 *
 * 마이크는 여기서 바로 enable되지만, 스피커는 채널만 만들어두고 enable하지
 * 않는다 — 실제로 재생할 때만 audio_io_speaker_enable()로 켤 것. TX DMA를
 * 켜두고 한 번도 안 쓰는 채로 방치하면 GDMA TX 인터럽트가 죽는 문제가
 * 실기기에서 확인됐다(2026-08-10).
 */
void audio_io_init(void);

/* 스피커 채널(I2S DMA + SD 핀)을 켠다/끈다. RX_AUDIO 진입/이탈, 또는
 * audio_io_play_beep() 호출 전후에만 쓸 것 — 부팅 시 자동으로 켜두지 않는
 * 이유는 audio_io_init() 주석 참고. disable은 이미 꺼진 상태에서 불러도
 * 안전(에러 무시). */
void audio_io_speaker_enable(void);
void audio_io_speaker_disable(void);

/*
 * PTT 눌렀을 때 "말하기 시작"을 알리는 짧은 2음 삐빅음을 재생한다(블로킹,
 * 총 ~210ms). 호출 전에 audio_io_speaker_enable()이 먼저 호출돼 있어야 한다.
 * RX_AUDIO(수신 재생)가 아직 미구현이라 앰프 배선/동작을 확인하기 위한
 * 테스트용 — 나중에 실제 음성 재생 붙으면 그대로 둬도 되고 빼도 된다.
 */
void audio_io_play_beep(void);

/*
 * 마이크에서 한 프레임(AUDIO_CODEC_FRAME_SAMPLES=160 샘플, 20ms)을 읽어
 * Speex로 인코딩한다. out/out_capacity는 audio_codec_encode()에 그대로 전달됨
 * (AUDIO_CODEC_MAX_ENCODED_BYTES 이상 권장).
 * 반환값: 인코딩된 바이트 수, 실패 시 -1 (I2S 타임아웃 포함).
 */
int audio_io_capture_encode(uint8_t *out, size_t out_capacity);

/*
 * 인코딩된 프레임 하나를 Speex로 디코딩해 스피커로 재생한다.
 * data/len: audio_codec_encode()가 만든 프레임 하나 분량.
 * 반환값: 성공 시 0, 실패 시 -1.
 */
int audio_io_decode_play(const uint8_t *data, size_t len);

#ifdef LOOPBACK_ENABLE
/*
 * TEMP(마이크 loopback 테스트 전용, LOOPBACK_ENABLE 꺼지면 통째로 빠짐):
 * audio_io_decode_play()와 동일하지만, 디코딩한 PCM에 gain 배율을 곱해
 * (1.0=원본, >1.0=증폭, <1.0=감쇠) 재생한다. int16 범위를 넘으면
 * 클리핑(saturate)한다. INMP441 캡처 신호가 원래 작아서(실측 peak
 * 846/32767 수준) 들리게 하려면 gain>1로 증폭 필요.
 */
int audio_io_decode_play_scaled(const uint8_t *data, size_t len, float gain);

/*
 * TEMP(마이크 loopback 진단용, LOOPBACK_ENABLE 꺼지면 통째로 빠짐): 프레임을
 * 디코딩만 하고 재생은 하지 않는다. 디코딩된 PCM의 최대 절댓값(피크,
 * 0~32767)을 반환 — 0에 가까우면 캡처 자체가 무음(배선/슬롯 매칭 문제)
 * 이라는 뜻, 크면 재생 쪽 문제로 좁혀짐. 실패 시 -1.
 */
int16_t audio_io_decode_peek_peak(const uint8_t *data, size_t len);
#endif /* LOOPBACK_ENABLE */

#ifdef __cplusplus
}
#endif
