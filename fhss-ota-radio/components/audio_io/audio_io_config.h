#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"

/* TEMP(마이크 loopback 테스트, 검증 끝나면 이 매크로와 관련 코드 전부 제거):
 * 이 매크로가 정의되어 있으면 MENU_IDLE에서 PTT로 mic->Speex->스피커
 * loopback 테스트(main/fsm.c의 mic_test_task 등)가 활성화된다.
 * audio_io.c/audio_io.h/main/fsm.c에서 공통으로 이 매크로를 본다 —
 * 켜려면 아래 줄의 주석만 해제. */
#define LOOPBACK_ENABLE

/* === 마이크 (INMP441, I2S RX 전용) — I2S_NUM_0 ===
 * L/R 핀은 3V3 고정 배선(우채널 고정, 2026-08-11 확정) -> 코드에서 SLOT_RIGHT로 수신. */
#define AUDIO_IO_MIC_I2S_PORT  I2S_NUM_0
#define AUDIO_IO_MIC_WS_GPIO   GPIO_NUM_4  /* WS / LRCLK */
#define AUDIO_IO_MIC_BCLK_GPIO GPIO_NUM_5  /* SCK */
#define AUDIO_IO_MIC_SD_GPIO   GPIO_NUM_6  /* 마이크 SD(데이터 출력) -> ESP 입력 */

/* === 스피커 (MAX98357A, I2S TX 전용) — I2S_NUM_1 ===
 * SD/GAIN은 GPIO로 직접 제어(더 이상 VDD/GND 직결 가정 아님).
 * SD: audio_io_speaker_enable()/disable()에서 HIGH/LOW로 켜고 끔.
 *     >1.4V(HIGH)="왼쪽 채널만 출력"이라 I2S_STD_SLOT_LEFT 설정과 맞음. <0.16V(LOW)=완전 꺼짐.
 * GAIN: 항상 HIGH(VDD) 고정 = 6dB — GPIO로 가능한 3가지(GND=12dB, 미연결=9dB, VDD=6dB) 중
 *       가장 낮은 볼륨. 저항 없이 GND/VDD/미연결로만 3, 6, 9, 12, 15dB 중 조합 가능(데이터시트 참고). */
/* 재배정(2026-08-11): GPIO9~14를 CC1101(SPI 4핀 + GDO0 + GDO2)용으로 통째로
 * 비우기 위해 앰프를 이동.
 * 재배정(2026-08-14): 그런데도 CC1101과 간섭이 있어 LRC/BCLK/DIN을 다시
 * GPIO7/15/16으로 이동(GAIN/SD는 17/18 그대로 유지). GPIO7/15/16은 CC1101
 * 핀 블록(9~14)과도, GAIN/SD(17/18)와도 안 겹침. */
#define AUDIO_IO_SPK_I2S_PORT  I2S_NUM_1
#define AUDIO_IO_SPK_BCLK_GPIO GPIO_NUM_15 /* SCK */
#define AUDIO_IO_SPK_WS_GPIO   GPIO_NUM_7  /* WS / LRC */
#define AUDIO_IO_SPK_DOUT_GPIO GPIO_NUM_16 /* ESP 출력 -> MAX98357A DIN */
#define AUDIO_IO_SPK_GAIN_GPIO GPIO_NUM_17
#define AUDIO_IO_SPK_SD_GPIO   GPIO_NUM_18

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
