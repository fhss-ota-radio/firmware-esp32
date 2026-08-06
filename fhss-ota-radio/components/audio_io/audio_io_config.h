#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"

/* === 마이크 (INMP441, I2S RX 전용) — I2S_NUM_0 ===
 * L/R 핀은 GND 고정 배선(좌채널 고정) 가정 -> 코드에서 SLOT_LEFT로 수신.
 * 실배선 확정 전 placeholder — 배선 정해지면 아래 GPIO 값만 수정. */
#define AUDIO_IO_MIC_I2S_PORT  I2S_NUM_0
#define AUDIO_IO_MIC_BCLK_GPIO GPIO_NUM_5  /* SCK */
#define AUDIO_IO_MIC_WS_GPIO   GPIO_NUM_6  /* WS / LRCLK */
#define AUDIO_IO_MIC_SD_GPIO   GPIO_NUM_7  /* 마이크 SD(데이터 출력) -> ESP 입력 */

/* === 스피커 (MAX98357A, I2S TX 전용) — I2S_NUM_1 ===
 * SD(SHUTDOWN) 핀은 VDD 직결(상시 활성) 배선 가정.
 * GPIO로 켜고 끄고 싶으면 AUDIO_IO_SPK_SD_GPIO를 정의하고 audio_io.c에서 사용할 것. */
#define AUDIO_IO_SPK_I2S_PORT  I2S_NUM_1
#define AUDIO_IO_SPK_BCLK_GPIO GPIO_NUM_15 /* SCK */
#define AUDIO_IO_SPK_WS_GPIO   GPIO_NUM_16 /* WS / LRCLK */
#define AUDIO_IO_SPK_DOUT_GPIO GPIO_NUM_17 /* ESP 출력 -> MAX98357A DIN */
// #define AUDIO_IO_SPK_SD_GPIO GPIO_NUM_18

/* DMA 버퍼 설정: 프레임 수 = audio_codec의 20ms 프레임(160 샘플)에 맞춤 */
#define AUDIO_IO_I2S_DMA_DESC_NUM  4
#define AUDIO_IO_I2S_DMA_FRAME_NUM 160

/* i2s_channel_read/write 최대 대기 시간(ms). 20ms/프레임보다 충분히 크게 잡아서
 * 정상 상황에서는 걸리지 않고, 배선/설정 문제 시 무한 대기 대신 -1을 반환하게 함. */
#define AUDIO_IO_I2S_TIMEOUT_MS 1000
