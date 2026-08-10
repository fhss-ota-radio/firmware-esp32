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
 * SD/GAIN은 GPIO로 직접 제어(더 이상 VDD/GND 직결 가정 아님).
 * SD: audio_io_speaker_enable()/disable()에서 HIGH/LOW로 켜고 끔.
 *     >1.4V(HIGH)="왼쪽 채널만 출력"이라 I2S_STD_SLOT_LEFT 설정과 맞음. <0.16V(LOW)=완전 꺼짐.
 * GAIN: 항상 HIGH(VDD) 고정 = 6dB — GPIO로 가능한 3가지(GND=12dB, 미연결=9dB, VDD=6dB) 중
 *       가장 낮은 볼륨. 저항 없이 GND/VDD/미연결로만 3, 6, 9, 12, 15dB 중 조합 가능(데이터시트 참고). */
#define AUDIO_IO_SPK_I2S_PORT  I2S_NUM_1
#define AUDIO_IO_SPK_BCLK_GPIO GPIO_NUM_15 /* SCK */
#define AUDIO_IO_SPK_WS_GPIO   GPIO_NUM_16 /* WS / LRCLK */
#define AUDIO_IO_SPK_DOUT_GPIO GPIO_NUM_17 /* ESP 출력 -> MAX98357A DIN */
#define AUDIO_IO_SPK_GAIN_GPIO GPIO_NUM_18
#define AUDIO_IO_SPK_SD_GPIO   GPIO_NUM_11

/* PTT 눌렀을 때 "말하기 시작" 알림 삐빅 소리. 볼륨은 GAIN(6dB, 위 참고)에
 * 더해 여기 진폭도 작게(풀스케일 32767의 약 1.5%) 잡아서 이중으로 낮춤 —
 * 스피커/앰프 실기기 테스트 초기 단계라 과음량 방지가 우선. 그래도 크면 이
 * 값을 더 낮추면 됨(0에 가까울수록 조용함). */
#define AUDIO_IO_BEEP_AMPLITUDE 500

/* DMA 버퍼 설정: 프레임 수 = audio_codec의 20ms 프레임(160 샘플)에 맞춤 */
#define AUDIO_IO_I2S_DMA_DESC_NUM  4
#define AUDIO_IO_I2S_DMA_FRAME_NUM 160

/* i2s_channel_read/write 최대 대기 시간(ms). 20ms/프레임보다 충분히 크게 잡아서
 * 정상 상황에서는 걸리지 않고, 배선/설정 문제 시 무한 대기 대신 -1을 반환하게 함. */
#define AUDIO_IO_I2S_TIMEOUT_MS 1000
