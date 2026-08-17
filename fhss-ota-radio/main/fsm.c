#include "fsm.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "audio_codec.h"
#include "audio_io.h"
#include "device_id.h"
#include "display_ui.h"
#include "fhss_audio_adapter.h"
#include "fhss_audio_pcm_test.h"
#include "ptt_button.h"
#include "rotary_encoder.h"
#include "status_led.h"

static const char *TAG = "fsm";

/* 마이크가 없어도 codec -> FHSS -> CC1101 -> codec -> speaker 전체 경로를
 * 실기기에서 확인하기 위한 임시 모드다. 0이면 기존 마이크 캡처를 사용한다. */
#define FHSS_AUDIO_PCM_TEST_ENABLED 0

static QueueHandle_t s_event_queue;
static fsm_state_t s_state = FSM_STATE_BOOT_INIT;

/*
 * 오디오 프레임 전용 큐. fsm_event_t(이벤트 큐)는 페이로드가 없는 enum이라
 * 데이터를 못 실으므로, 수신 프레임 바이트는 이 큐로 따로 옮기고 이벤트
 * 큐에는 "도착했다"는 신호(FSM_EVENT_RX_FRAME)만 올린다.
 *
 * 재배정(2026-08-17): 깊이 4(80ms 버퍼)였을 때 실기기 FHSS RX 테스트에서
 * "rx audio queue full, dropping frame"이 반복되며 재생이 끊기는(두두두
 * 소리) 문제가 있었음 — MENU_COMM -> RX_AUDIO 전이 직후 rx_audio_task가
 * 실제로 큐를 비우기 시작하기 전 짧은 구간에 프레임이 몰려서 초반에 버스트로
 * 드롭되는 것으로 보임. 아이템 하나가 몇십 바이트 수준이라 메모리 부담은
 * 무시할 만해서, 그 초기 버스트를 넉넉히 흡수하도록 32개(20ms/프레임 기준
 * 약 640ms 버퍼)로 올림.
 */
#define FSM_RX_AUDIO_FRAME_MAX_BYTES AUDIO_CODEC_MAX_ENCODED_BYTES
#define FSM_RX_AUDIO_QUEUE_DEPTH     32

/* 이 시간 동안 새 프레임이 안 오면 수신이 끝난 것으로 보고 FSM_EVENT_RX_DONE을
 * 스스로 올린다 (무음/타임아웃 기반 종료 — 결정 근거는 docs/fsm-design.md 참고). */
#define FSM_RX_AUDIO_IDLE_TIMEOUT_MS 1000

typedef struct {
    uint8_t data[FSM_RX_AUDIO_FRAME_MAX_BYTES];
    size_t len;
} fsm_rx_audio_frame_t;

static QueueHandle_t s_rx_audio_queue;

/* RF 태스크에서 Speex decode/I2S 재생까지 실행하면 다음 홉 수신이 늦어진다.
 * 따라서 수신 frame은 팀 FSM이 이미 소유한 오디오 큐로 넘겨 RX 태스크가 재생한다. */
static bool on_fhss_rx_audio_frame(const uint8_t *frame, size_t length, void *context)
{
    (void)context;
    return fsm_post_rx_audio_frame(frame, length);
}

/* 정상적인 슬롯 보정은 FHSS 내부에서 처리하고, 완전히 추종을 놓치거나 RF 계층이
 * 복구 불가능할 때만 팀 FSM의 전역 안전장치 이벤트로 변환한다. */
static void on_fhss_audio_event(fhss_audio_adapter_event_t event, void *context)
{
    (void)context;
    fsm_post_event(event == FHSS_AUDIO_ADAPTER_EVENT_SYNC_LOST
        ? FSM_EVENT_SYNC_LOST
        : FSM_EVENT_ERROR);
}

/* 상태별 이름/이벤트별 이름: 로그 및 OLED 표시용 */
static const char *s_state_names[FSM_STATE_COUNT] = {
    [FSM_STATE_BOOT_INIT]     = "BOOT_INIT",
    [FSM_STATE_MENU_COMM]     = "MENU_COMM",
    [FSM_STATE_MENU_IDLE]     = "MENU_IDLE",
    [FSM_STATE_MENU_OTA]      = "MENU_OTA",
    [FSM_STATE_TX_AUDIO]      = "TX_AUDIO",
    [FSM_STATE_RX_AUDIO]      = "RX_AUDIO",
    [FSM_STATE_OTA_RECEIVING] = "OTA_RECEIVING",
    [FSM_STATE_OTA_APPLYING]  = "OTA_APPLYING",
    [FSM_STATE_ERROR]         = "ERROR",
};

static const char *s_event_names[FSM_EVENT_COUNT] = {
    [FSM_EVENT_INIT_DONE]      = "INIT_DONE",
    [FSM_EVENT_SYNC_LOST]      = "SYNC_LOST",
    [FSM_EVENT_MENU_SELECT_COMM] = "MENU_SELECT_COMM",
    [FSM_EVENT_MENU_SELECT_IDLE] = "MENU_SELECT_IDLE",
    [FSM_EVENT_MENU_SELECT_OTA]  = "MENU_SELECT_OTA",
    [FSM_EVENT_PTT_PRESS]      = "PTT_PRESS",
    [FSM_EVENT_PTT_RELEASE]    = "PTT_RELEASE",
    [FSM_EVENT_RX_FRAME]       = "RX_FRAME",
    [FSM_EVENT_RX_DONE]        = "RX_DONE",
    [FSM_EVENT_OTA_START]      = "OTA_START",
    [FSM_EVENT_OTA_CHUNK]      = "OTA_CHUNK",
    [FSM_EVENT_OTA_COMPLETE]   = "OTA_COMPLETE",
    [FSM_EVENT_OTA_VERIFY_OK]  = "OTA_VERIFY_OK",
    [FSM_EVENT_OTA_VERIFY_FAIL] = "OTA_VERIFY_FAIL",
    [FSM_EVENT_ERROR]          = "ERROR",
    [FSM_EVENT_RETRY]          = "RETRY",
};

/*
 * 상태별 전이. 전역 전이(EV_ERROR, EV_SYNC_LOST)는 fsm_task()에서 별도 처리하므로 여기 넣지 않는다.
 *
 * 브로드캐스트 방식이라 "동기 획득 대기" 상태가 없다 — PTT 누른 쪽이 정해진
 * 시작 채널로 먼저 송신하고 그 순간부터 시드 기반으로 제멋대로 호핑하며,
 * 받는 쪽은 그 수신 시점을 기준으로 같은 시드로 호핑을 추종한다. 그래서
 * BOOT_INIT 이후엔 곧바로 MENU_COMM(정해진 채널에서 통신 대기)으로 들어간다.
 *
 * EV_SYNC_LOST는 그래도 전역 안전장치로 남겨둔다 — 무선 계층(rf_transport/
 * fhss_core)이 홉 추종 중 연속 미수신 등으로 타이밍이 완전히 깨졌다고
 * 판단하면 이 이벤트를 올리고, FSM은 지금 어느 상태에 있든 MENU_COMM(정해진
 * 채널)으로 강제 복귀한다. 정상적인 한 세션 종료(TX_AUDIO의 PTT_RELEASE,
 * RX_AUDIO의 RX_DONE)와는 별개의, 진짜 이상 상황용 탈출구다.
 *
 * 음성(FHSS)과 OTA는 같은 CC1101 라디오를 쓰는 단일 반이중 트랜시버이므로,
 * OTA_RECEIVING 진입 시 음성 호핑 이탈은 정책이 아니라 하드웨어 제약이다.
 *
 * MENU_COMM/MENU_IDLE/MENU_OTA는 수신 패킷 해석을 게이팅하는 3-way 메뉴
 * 모드다(2026-08-11 UI 재설계로 MENU_IDLE이 "뮤트"로 새로 추가됨 — 기존
 * MENU_IDLE은 MENU_COMM으로 개명). MENU_COMM=음성 통신, MENU_OTA=펌웨어
 * 청크, MENU_IDLE=뮤트(수신 처리 없음, PTT/RX_FRAME 전이도 없음). 메뉴 전환
 * 이벤트(MENU_SELECT_COMM/IDLE/OTA)는 이 세 상태끼리만 정의돼있고 TX_AUDIO/
 * RX_AUDIO/OTA_RECEIVING/OTA_APPLYING에는 없다 — 그 상태에서 메뉴 전환
 * 이벤트가 오면 unhandled로 무시되며, 이게 곧 "활동 중 메뉴 변경 불가"의
 * 구현이다. 같은 이유로 OTA_START는 MENU_OTA에서만, PTT_PRESS/RX_FRAME은
 * MENU_COMM에서만 유효해서 서로 끼어들 수 없다.
 */
typedef struct {
    fsm_state_t state;
    fsm_event_t event;
    fsm_state_t next_state;
} fsm_transition_t;

static const fsm_transition_t s_transitions[] = {
    { FSM_STATE_BOOT_INIT,     FSM_EVENT_INIT_DONE,      FSM_STATE_MENU_COMM },

    { FSM_STATE_MENU_COMM,     FSM_EVENT_PTT_PRESS,      FSM_STATE_TX_AUDIO },
    { FSM_STATE_MENU_COMM,     FSM_EVENT_RX_FRAME,       FSM_STATE_RX_AUDIO },
    { FSM_STATE_MENU_COMM,     FSM_EVENT_MENU_SELECT_IDLE, FSM_STATE_MENU_IDLE },
    { FSM_STATE_MENU_COMM,     FSM_EVENT_MENU_SELECT_OTA, FSM_STATE_MENU_OTA },

    { FSM_STATE_MENU_IDLE,     FSM_EVENT_MENU_SELECT_COMM, FSM_STATE_MENU_COMM },
    { FSM_STATE_MENU_IDLE,     FSM_EVENT_MENU_SELECT_OTA, FSM_STATE_MENU_OTA },

    { FSM_STATE_MENU_OTA,      FSM_EVENT_MENU_SELECT_COMM, FSM_STATE_MENU_COMM },
    { FSM_STATE_MENU_OTA,      FSM_EVENT_MENU_SELECT_IDLE, FSM_STATE_MENU_IDLE },
    { FSM_STATE_MENU_OTA,      FSM_EVENT_OTA_START,      FSM_STATE_OTA_RECEIVING },

    { FSM_STATE_TX_AUDIO,      FSM_EVENT_PTT_RELEASE,    FSM_STATE_MENU_COMM },

    /* RF packet 하나의 두 Speex frame이 연속 도착해도 두 번째 RX_FRAME이
     * 현재 RX 태스크를 재시작하지 않고 정상적으로 큐에 누적되게 한다. */
    { FSM_STATE_RX_AUDIO,      FSM_EVENT_RX_FRAME,       FSM_STATE_RX_AUDIO },
    { FSM_STATE_RX_AUDIO,      FSM_EVENT_RX_DONE,        FSM_STATE_MENU_COMM },

    { FSM_STATE_OTA_RECEIVING, FSM_EVENT_OTA_CHUNK,      FSM_STATE_OTA_RECEIVING },
    { FSM_STATE_OTA_RECEIVING, FSM_EVENT_OTA_COMPLETE,   FSM_STATE_OTA_APPLYING },

    { FSM_STATE_OTA_APPLYING,  FSM_EVENT_OTA_VERIFY_OK,   FSM_STATE_BOOT_INIT },
    { FSM_STATE_OTA_APPLYING,  FSM_EVENT_OTA_VERIFY_FAIL, FSM_STATE_MENU_OTA },

    { FSM_STATE_ERROR,         FSM_EVENT_RETRY,          FSM_STATE_BOOT_INIT },
};

/*
 * UI 컴포넌트(display_ui/ptt_button/rotary_encoder) 콜백 — 각 컴포넌트는 FSM을
 * 모르므로, 이 콜백들이 fsm_post_event()로 매핑하는 접착부 역할을 한다.
 * rf_transport 등 아직 없는 컴포넌트가 직접 호출해야 할 지점(fsm_post_rx_audio_frame
 * 등)은 그대로 TODO로 남겨둔다 — 지금 여기서 채우면 실제 API가 없는 상태라
 * 추측성 코드가 되고, 나중에 다시 갈아엎어야 한다.
 */
/*
 * TEMP(INMP441 실배선 테스트용, LOOPBACK_ENABLE로 켜고 끔 —
 * audio_io_config.h의 매크로 정의 참고): MENU_IDLE(뮤트)에서 PTT를 누르면
 * 정식 전이표를 타지 않고(MENU_IDLE은 설계상 PTT 전이가 없음) 이 loopback
 * 테스트로만 빠진다 — 삐빅음 없이 마이크로 캡처한 프레임을 Speex로
 * 인코딩/디코딩 왕복시켜 버퍼에 모아뒀다가 PTT를 떼면 1초 뒤 재생한다.
 * INMP441 실측 캡처 신호가 원래 작아서(peak 846/32767 수준, 2026-08-11
 * 실기기 확인) 그냥 재생하면 안 들려서 소프트웨어 gain으로 증폭 후
 * 재생한다(MIC_TEST_PLAYBACK_GAIN). 정식 FSM 상태/전이는 전혀 건드리지
 * 않아 LOOPBACK_ENABLE을 끄면(기본값) 원래 동작으로 돌아간다.
 */
#ifdef LOOPBACK_ENABLE
#define MIC_TEST_MAX_FRAMES 250 /* 250 * 20ms = 5초 상한 */
#define MIC_TEST_PLAYBACK_GAIN 16.0f /* peak 846 기준 약 13536(풀스케일 41%)까지 증폭 */

typedef struct {
    uint8_t data[AUDIO_CODEC_MAX_ENCODED_BYTES];
    size_t len;
} mic_test_frame_t;

static TaskHandle_t s_mic_test_task;
static volatile bool s_mic_test_recording;

static void mic_test_task(void *arg)
{
    mic_test_frame_t *frames = malloc(sizeof(mic_test_frame_t) * MIC_TEST_MAX_FRAMES);
    size_t frame_count = 0;

    while (s_mic_test_recording && frame_count < MIC_TEST_MAX_FRAMES) {
        uint8_t enc[AUDIO_CODEC_MAX_ENCODED_BYTES];
        int n = audio_io_capture_encode(enc, sizeof(enc));
        if (n > 0) {
            memcpy(frames[frame_count].data, enc, (size_t)n);
            frames[frame_count].len = (size_t)n;
            frame_count++;
        }
    }

    /* TEMP(진단용): 캡처된 신호가 실제로 있는지(0에 가까우면 배선/슬롯 문제)
     * 재생 전에 피크로 먼저 확인 */
    int16_t peak = 0;
    for (size_t i = 0; i < frame_count; i++) {
        int16_t p = audio_io_decode_peek_peak(frames[i].data, frames[i].len);
        if (p > peak) {
            peak = p;
        }
    }
    ESP_LOGI(TAG, "mic test: %u frames captured, peak=%d/32767, playback in 1s",
             (unsigned)frame_count, peak);
    vTaskDelay(pdMS_TO_TICKS(1000));

    audio_io_speaker_enable();
    for (size_t i = 0; i < frame_count; i++) {
        audio_io_decode_play_scaled(frames[i].data, frames[i].len, MIC_TEST_PLAYBACK_GAIN);
    }
    audio_io_speaker_disable();

    free(frames);
    s_mic_test_task = NULL;
    vTaskDelete(NULL);
}
#endif /* LOOPBACK_ENABLE */

static void on_ptt_event(bool pressed, void *ctx)
{
    fsm_state_t state = fsm_get_state();

#ifdef LOOPBACK_ENABLE
    /* TEMP: MENU_IDLE에서는 정식 FSM 이벤트 대신 마이크 loopback 테스트로 라우팅.
     * 이것도 실제로 마이크를 캡처하는 "수음"이라 흰색 LED를 그대로 켠다. */
    if (state == FSM_STATE_MENU_IDLE) {
        if (pressed) {
            status_led_set_white_dim();
            if (s_mic_test_task == NULL) {
                s_mic_test_recording = true;
                xTaskCreate(mic_test_task, "mic_test", 8192, NULL, tskIDLE_PRIORITY + 3, &s_mic_test_task);
            }
        } else {
            status_led_off();
            s_mic_test_recording = false;
        }
        return;
    }
#endif /* LOOPBACK_ENABLE */

    /* status_led 흰색은 실제로 수음(TX_AUDIO 캡처)으로 이어지는 PTT일 때만
     * 켠다 — 전이표상 MENU_COMM에서 누른 PTT만 TX_AUDIO로 이어지고, 다른
     * 상태(예: MENU_OTA)에서 누르면 FSM이 그냥 무시하니 LED도 안 켜야
     * 맞다(이전엔 PTT 원시 입력을 상태와 무관하게 그대로 반영해서, 아무
     * 효과도 없는 상태에서 눌러도 흰색이 켜지는 문제가 있었음). 끌 때는
     * 반대로 상태와 무관하게 항상 꺼서, EV_ERROR/EV_SYNC_LOST 같은 전역
     * 전이로 도중에 TX_AUDIO를 벗어나도 흰색이 켜진 채로 안 남게 한다. */
    if (pressed) {
        if (state == FSM_STATE_MENU_COMM) {
            status_led_set_white_dim();
        }
    } else {
        status_led_off();
    }

    fsm_post_event(pressed ? FSM_EVENT_PTT_PRESS : FSM_EVENT_PTT_RELEASE);
}

/* rotary_encoder_menu_t <-> display_ui_menu_item_t는 순서가 같은 3-way라
 * 매핑이 자명하지만, 열거형이 나중에 각자 따로 바뀔 수 있으니 캐스트 대신
 * 명시적으로 변환한다. */
static display_ui_menu_item_t menu_item_from_rotary(rotary_encoder_menu_t cursor)
{
    switch (cursor) {
        case ROTARY_ENCODER_MENU_COMM: return DISPLAY_UI_MENU_COMM;
        case ROTARY_ENCODER_MENU_OTA:  return DISPLAY_UI_MENU_OTA;
        default:                       return DISPLAY_UI_MENU_IDLE;
    }
}

/* 현재 FSM 상태를 화면에 표시할 메뉴 항목으로 변환한다. TX_AUDIO/RX_AUDIO는
 * MENU_COMM에서만, OTA_RECEIVING/OTA_APPLYING은 MENU_OTA에서만 진입 가능하니
 * (전이표 참고) 그 상태들도 각각 COMM/OTA로 매핑하는 게 실제로 맞다. */
static display_ui_menu_item_t menu_item_from_fsm_state(fsm_state_t state)
{
    switch (state) {
        case FSM_STATE_MENU_COMM:
        case FSM_STATE_TX_AUDIO:
        case FSM_STATE_RX_AUDIO:
            return DISPLAY_UI_MENU_COMM;
        case FSM_STATE_MENU_IDLE:
            return DISPLAY_UI_MENU_IDLE;
        case FSM_STATE_MENU_OTA:
        case FSM_STATE_OTA_RECEIVING:
        case FSM_STATE_OTA_APPLYING:
            return DISPLAY_UI_MENU_OTA;
        default:
            return DISPLAY_UI_MENU_COMM;
    }
}

static void on_menu_select(rotary_encoder_menu_t selected, void *ctx)
{
    fsm_event_t event;
    switch (selected) {
        case ROTARY_ENCODER_MENU_COMM: event = FSM_EVENT_MENU_SELECT_COMM; break;
        case ROTARY_ENCODER_MENU_OTA:  event = FSM_EVENT_MENU_SELECT_OTA;  break;
        default:                       event = FSM_EVENT_MENU_SELECT_IDLE; break;
    }
    fsm_post_event(event);
}

/* 로터리 회전만으로는 FSM 이벤트가 없다 (docs/fsm-design.md §메뉴 게이팅) —
 * 클릭으로 확정하기 전까지는 화면에 미리보기(흰 테두리)만 갱신한다. */
static void on_menu_cursor(rotary_encoder_menu_t cursor, void *ctx)
{
    display_ui_draw_menu(menu_item_from_fsm_state(fsm_get_state()), menu_item_from_rotary(cursor));
}

/*
 * TX_AUDIO 동안만 도는 캡처 태스크. PTT를 누르고 있는 동안(TX_AUDIO 상태인
 * 동안) 20ms마다 마이크를 읽어 Speex로 인코딩한다. rf_transport가 아직 없어
 * 인코딩된 프레임을 실제로 보낼 곳이 없으므로 그 부분만 TODO — 캡처/인코딩
 * 자체는 audio_io가 이미 있으니 추측 없이 그대로 동작한다.
 * MENU_COMM 진입(PTT_RELEASE) 시 on_enter_menu_comm()에서 태스크를 정리한다.
 *
 * "말하기 시작" 삐빅음은 여기(태스크 안)가 아니라 on_enter_tx_audio()에서,
 * 즉 이 태스크를 만들기 *전에* fsm_task 안에서 재생한다 — PTT를 아주 빠르게
 * 누르고 뗐을 때(삐빅음 재생 도중 PTT_RELEASE가 들어오는 경우)
 * vTaskDelete()가 이 태스크를 스피커 disable 전에 강제 종료시켜서 SD 핀이
 * HIGH로 계속 남아 삐빅음이 안 끝나고 계속 재생되는 버그가 있었다
 * (troubleshoot/요약.md, 2026-08-10). fsm_task는 이벤트를 한 번에 하나씩
 * 순서대로 처리하므로, 삐빅음 재생을 fsm_task 안에서 끝까지 블로킹으로 마친
 * 뒤에야 다음 이벤트(PTT_RELEASE)를 처리하게 만들어서 이 경쟁 상태를 없앴다.
 */
static TaskHandle_t s_tx_audio_task;
/* TEMP였던 vTaskDelete(s_tx_audio_task) 강제 종료를 없애기 위한 협조적
 * 종료 플래그 — stop_tx_audio_task() 참고. */
static volatile bool s_tx_audio_should_stop;

static void tx_audio_task(void *arg)
{
#if FHSS_AUDIO_PCM_TEST_ENABLED
    /* 160-sample PCM을 20 ms cadence로 생성해 실제 codec/RF 경로에 공급한다.
     * 태스크 수명은 마이크 경로와 동일하게 PTT PRESS~RELEASE로 제한된다. */
    fhss_audio_pcm_test_run();
#else
    uint8_t frame[AUDIO_CODEC_MAX_ENCODED_BYTES];

    while (!s_tx_audio_should_stop) {
        int n = audio_io_capture_encode(frame, sizeof(frame));
        if (n > 0) {
            /* 20 ms Speex frame 두 개를 adapter가 RF packet 하나로 묶는다. */
            if (!fhss_audio_adapter_submit_encoded_frame(frame, (size_t)n)) {
                ESP_LOGW(TAG, "encoded audio frame dropped: length=%d", n);
            }
        }
    }
#endif

    s_tx_audio_task = NULL;
    vTaskDelete(NULL);
}

/*
 * PTT_RELEASE/EV_ERROR/EV_SYNC_LOST로 TX_AUDIO를 벗어날 때 이 함수로
 * s_tx_audio_task를 정리한다. 예전엔 vTaskDelete(s_tx_audio_task)로 그
 * 자리에서 바로 죽였는데, `#else`(실제 마이크) 경로는 거의 항상
 * audio_io_capture_encode() -> i2s_channel_read(mic 채널) 안에 블로킹돼
 * 있어서, 하필 그 순간 강제 종료되면 I2S 드라이버가 그 채널을 "읽는 중"
 * 상태로 표시해둔 채 못 풀려서 이후 그 채널로의 모든 read가
 * ESP_ERR_INVALID_STATE(259)로 실패하는 버그가 있었다(마이크 loopback
 * 테스트에서 "mic read failed (err=259)"로 재현됨 — 스피커/삐빅음 쪽의
 * 동일 계열 버그를 고쳤던 troubleshoot/on_enter_tx_audio-beep_fix.md와
 * 같은 원인, 이쪽(캡처 루프 본체)은 그때 안 고쳐져 있었음).
 * `rx_audio_task`가 이미 쓰고 있는 "태스크가 스스로 끝낸다" 패턴을 여기도
 * 적용 — 큐 대기(rx_audio_task)와 달리 I2S read는 강제 종료해도 안전한
 * 자원이 아니라서, 플래그만 세우고 태스크가 다음 루프에서 스스로
 * vTaskDelete(NULL)하도록 기다린다. 정상 상황(마이크에서 데이터가 계속
 * 들어옴)이면 다음 20ms 프레임 주기 안에 곧바로 끝나고, 데이터가 전혀 안
 * 들어와도 i2s_channel_read()의 자체 타임아웃(AUDIO_IO_I2S_TIMEOUT_MS,
 * 1초)이 상한이라 무한 대기는 아니다.
 */
static void stop_tx_audio_task(void)
{
    if (s_tx_audio_task == NULL) {
        return;
    }
    s_tx_audio_should_stop = true;
    while (s_tx_audio_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_tx_audio_should_stop = false;
}

/*
 * RX_AUDIO 동안만 도는 재생 태스크. s_rx_audio_queue에서 프레임을 꺼내
 * audio_io_decode_play()로 재생한다. FSM_RX_AUDIO_IDLE_TIMEOUT_MS(1초) 동안
 * 새 프레임이 안 들어오면 수신이 끝난 것으로 보고 FSM_EVENT_RX_DONE을 스스로
 * 올린 뒤 태스크를 종료한다 — PTT_RELEASE 같은 명시적 종료 신호가 RX 쪽엔
 * 없어서 무음/타임아웃 기반으로 정한 것(값은 1초로 확정, 추후 실측 후 조정 가능).
 *
 * s_rx_audio_task를 NULL로 되돌리고 나서 vTaskDelete(NULL)로 자기 자신을
 * 지운다 — on_enter_menu_comm()이 FSM_EVENT_RX_DONE 처리 시 다시 한번
 * s_rx_audio_task를 정리하려 하는데, 이미 NULL이라 아무 일도 안 하고
 * 넘어간다(이중 삭제 방지).
 *
 * 여전히 미정: fsm_post_rx_audio_frame()을 실제로 호출해줄 rf_transport가
 * 아직 없어서, 지금은 이 타임아웃이 "부팅 후 곧바로 RX_AUDIO를 빠져나온다"는
 * 뜻밖에 안 됨 — rf_transport가 생겨서 실제 프레임이 들어오기 시작해야 이
 * 타임아웃이 의미를 가진다.
 */
static TaskHandle_t s_rx_audio_task;
/* I2S write 또는 queue wait 도중 RX 태스크를 외부에서 강제 삭제하지 않는다.
 * 정상 timeout이 아닌 상태 전이에서 stop_rx_audio_task()가 이 플래그를 세우면
 * RX 태스크가 현재 작업을 마친 뒤 speaker channel까지 직접 정리한다. */
static volatile bool s_rx_audio_should_stop;

/* 새 프레임이 없는 idle 구간에서 xQueueReceive를 짧게 끊어 폴링하며
 * audio_io_write_silence()로 무음을 채워 넣는 주기(ms). 재배정(2026-08-17):
 * 예전엔 FSM_RX_AUDIO_IDLE_TIMEOUT_MS(1초)를 한 번에 블로킹 대기해서 그
 * 동안 I2S write가 전혀 없었는데, I2S TX가 circular DMA라 write가 멈추면
 * 마지막 실제 음성 프레임 파형을 그대로 반복 재생 -> "두두두두" 잡음으로
 * 들리는 문제가 실기기에서 확인됨. 프레임 주기(20ms)와 맞춰 폴링. */
#define FSM_RX_AUDIO_POLL_MS 20U

static void rx_audio_task(void *arg)
{
    fsm_rx_audio_frame_t frame;

    bool timed_out = false;
    uint32_t idle_ms = 0U;

    while (!s_rx_audio_should_stop) {
        if (xQueueReceive(s_rx_audio_queue, &frame, pdMS_TO_TICKS(FSM_RX_AUDIO_POLL_MS)) == pdTRUE) {
            audio_io_decode_play(frame.data, frame.len);
            idle_ms = 0U;
        } else {
            audio_io_write_silence();
            idle_ms += FSM_RX_AUDIO_POLL_MS;
            if (idle_ms >= FSM_RX_AUDIO_IDLE_TIMEOUT_MS) {
                timed_out = true;
                break;
            }
        }
    }

    /* speaker를 enable한 RX 태스크가 disable까지 책임진다. 핸들을 먼저 NULL로
     * 만들면 MENU_COMM 진입 코드가 정리를 건너뛰어 다음 RX에서 이미 실행 중인
     * I2S channel을 다시 enable하는 ESP_ERR_INVALID_STATE가 발생한다. */
    audio_io_speaker_disable();
    s_rx_audio_task = NULL;
    if (timed_out && !s_rx_audio_should_stop) {
        fsm_post_event(FSM_EVENT_RX_DONE);
    }
    vTaskDelete(NULL);
}

/* SYNC_LOST/ERROR처럼 RX_AUDIO를 외부 이벤트로 벗어날 때도 I2S write 중인
 * 태스크를 vTaskDelete()로 끊지 않는다. 최대 queue timeout(1초) 뒤 태스크가
 * speaker를 정상 disable하고 스스로 종료할 때까지 기다린다. */
static void stop_rx_audio_task(void)
{
    if (s_rx_audio_task == NULL) {
        return;
    }

    s_rx_audio_should_stop = true;
    while (s_rx_audio_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_rx_audio_should_stop = false;
}

/* BOOT_INIT은 최초 부팅뿐 아니라 ERROR 상태의 EV_RETRY로도 재진입한다
 * (s_transitions의 유일한 탈출구). 아래 하드웨어 init 함수들은 전부 "한 번만
 * 불림"을 가정하고 있어(예: I2C 버스/GPIO ISR을 매번 새로 등록) 재진입 시
 * 그대로 다시 부르면 이미 등록된 리소스에 충돌해 실패할 수 있다 — 그래서
 * s_boot_init_done 플래그로 감싸 최초 1회만 실행되게 한다. */
static bool s_boot_init_done;

/* 상태별 진입 동작. 실제 하드웨어 제어는 각 담당(TODO)이 채운다. */
static void on_enter_boot_init(void)
{
    char id_hex[DEVICE_ID_LEN * 2 + 1];
    device_id_get_hex(id_hex, sizeof(id_hex));
    ESP_LOGI(TAG, "device id: %s", id_hex);

    if (!s_boot_init_done) {
        display_ui_init();
        status_led_init();
    }

    /* ERROR -> RETRY로 여기 다시 들어왔을 수도 있으니(최초 부팅이 아닌
     * 경우), on_enter_error()에서 켠 빨간 점멸/OLED "ERROR" 표시를 확실히
     * 정리한다 — 둘 다 안 켜져 있었어도 안전(에러 무시). 최초 부팅이든
     * 재진입이든 이 시점엔 이미 display_ui_init()/status_led_init()이
     * 호출된 뒤라(위 분기든 이전 진입이든) s_strip/OLED 핸들이 항상 존재해
     * 안전하다. */
    status_led_stop_error_blink();
    display_ui_clear_status();

    if (!s_boot_init_done) {
        ptt_button_init();
        ptt_button_set_callback(on_ptt_event, NULL);

        rotary_encoder_init();
        rotary_encoder_set_select_callback(on_menu_select, NULL);
        rotary_encoder_set_cursor_callback(on_menu_cursor, NULL);

        audio_codec_init();
        audio_io_init();

        /* Main이 무선 레지스터/홉 시간을 직접 소유하지 않도록 adapter 경계에서
         * CC1101/FHSS 서비스를 시작하고 수신 frame 및 전역 오류만 FSM에 전달한다. */
        const fhss_audio_adapter_config_t fhss_audio_config = {
            .rx_frame_callback = on_fhss_rx_audio_frame,
            .event_callback = on_fhss_audio_event,
            .callback_context = NULL,
        };
        if (!fhss_audio_adapter_init(&fhss_audio_config)) {
            ESP_LOGE(TAG, "FHSS audio adapter initialization failed");
            fsm_post_event(FSM_EVENT_ERROR);
        }

        s_boot_init_done = true;
    }
}
/* MENU_COMM: 통신 대기(기본 메뉴). TX_AUDIO/RX_AUDIO는 여기서만 나가고
 * 여기로만 돌아오니, 오디오 태스크 정리는 전부 여기서 한다(이전엔
 * on_enter_menu_idle이라는 이름이었음 — 2026-08-11에 개명, 새 MENU_IDLE은
 * 완전히 다른 뮤트 상태). */
static void on_enter_menu_comm(void)
{
    if (s_tx_audio_task != NULL) {
        stop_tx_audio_task();
#if FHSS_AUDIO_PCM_TEST_ENABLED
        fhss_audio_pcm_test_stats_t stats = {0};
        fhss_audio_pcm_test_get_stats(&stats);
        ESP_LOGI(TAG,
                 "PCM TEST STOP generated=%lu encoded=%lu submitted=%lu "
                 "encode_fail=%lu submit_fail=%lu interval_us[min/max]=%lld/%lld",
                 (unsigned long)stats.generated_frames,
                 (unsigned long)stats.encoded_frames,
                 (unsigned long)stats.submitted_frames,
                 (unsigned long)stats.encode_failures,
                 (unsigned long)stats.submit_failures,
                 (long long)stats.minimum_interval_us,
                 (long long)stats.maximum_interval_us);
#endif
    }
    /* PTT RELEASE 뒤 남은 frame을 마무리하고 CC1101을 시작 채널 RX로 복귀시킨다. */
    if (!fhss_audio_adapter_end_tx()) {
        ESP_LOGW(TAG, "failed to finish FHSS audio TX session cleanly");
    }
    /* on_enter_rx_audio()에서 켠 sky-blue RX 표시등을 여기서 끈다 — 실제
     * 정리(speaker disable 등)는 stop_rx_audio_task()의 협조적 종료가
     * 담당하므로 LED만 별도로 끈다. */
    if (s_rx_audio_task != NULL) {
        status_led_off();
    }
    stop_rx_audio_task();
    display_ui_draw_menu(DISPLAY_UI_MENU_COMM, menu_item_from_rotary(rotary_encoder_get_cursor()));
    display_ui_set_status_scroll("HOLD PTT TO SPEAK");
}

/* MENU_IDLE(뮤트): TX_AUDIO/RX_AUDIO로 들어오는 전이가 없어서(전이표 참고)
 * 오디오 태스크가 실행 중일 수 없다 — 정리할 게 없다. */
static void on_enter_menu_idle(void)
{
    display_ui_draw_menu(DISPLAY_UI_MENU_IDLE, menu_item_from_rotary(rotary_encoder_get_cursor()));
#ifdef LOOPBACK_ENABLE
    display_ui_set_status_scroll("PRESS PTT TO TEST LOOPBACK");
#else
    display_ui_set_status("MUTED");
#endif
}

static void on_enter_menu_ota(void)
{
    display_ui_draw_menu(DISPLAY_UI_MENU_OTA, menu_item_from_rotary(rotary_encoder_get_cursor()));
    /* 글자 수는 화면 폭(8자)에 다 들어가지만, "대기 중"임을 시각적으로
     * 드러내려고 일부러 흐르는 문구로 표시(display_ui_set_status_scroll()). */
    display_ui_set_status_scroll("STANDBY");
    /* TODO(팀2): CC1101 OTA 채널 리스닝 준비. */
}
static void on_enter_tx_audio(void)
{
    display_ui_set_status_animated("TX");

    /* fsm_task 안에서 블로킹으로 끝까지 재생 — 이유는 tx_audio_task 주석 참고. */
    audio_io_speaker_enable();
    audio_io_play_beep();
    audio_io_speaker_disable();

    /* 첫 Speex frame이 RX 역할 서비스로 들어가 유실되지 않도록 캡처 태스크보다
     * 먼저 CC1101/FHSS 서비스를 TX 세션으로 전환한다. */
    if (!fhss_audio_adapter_begin_tx()) {
        ESP_LOGE(TAG, "failed to start FHSS audio TX session");
        fsm_post_event(FSM_EVENT_ERROR);
        return;
    }

#if FHSS_AUDIO_PCM_TEST_ENABLED
    ESP_LOGW(TAG, "PCM TEST MODE: microphone bypassed; real Speex/FHSS/RF path active");
#endif

    /* 스택 8192 — audio_io_capture_encode() -> audio_codec_encode() ->
     * speex_encode_int()(LPC 분석/코드북 탐색) 호출 체인이 4096으론 빠듯해서
     * 여유 있게 늘림. (실기기에서 겪은 재부팅 크래시의 실제 원인은 이게
     * 아니라 스피커 TX DMA 미사용 방치였음 — audio_io.c 주석 참고. 이 스택
     * 크기는 예방적으로 유지.) */
    xTaskCreate(tx_audio_task, "tx_audio", 8192, NULL, tskIDLE_PRIORITY + 3, &s_tx_audio_task);
}
static void on_enter_rx_audio(void)
{
    display_ui_set_status_animated("RX");
    status_led_set_sky_blue_dim();

    /* audio_codec_decode()도 같은 호출 체인 무게라 tx와 동일하게 8192로. */
    audio_io_speaker_enable();
    if (xTaskCreate(rx_audio_task, "rx_audio", 8192, NULL,
                    tskIDLE_PRIORITY + 3, &s_rx_audio_task) != pdPASS) {
        audio_io_speaker_disable();
        s_rx_audio_task = NULL;
        ESP_LOGE(TAG, "failed to create RX audio task");
        fsm_post_event(FSM_EVENT_ERROR);
    }
}
/* OTA_START 수신(OTA_CLIENT_EVENT_STARTED -> FSM_EVENT_OTA_START)으로 여기
 * 진입하는 순간 STANDBY 스크롤 문구를 진행 표시로 바꾼다. 이 시점엔 아직
 * 첫 청크가 안 왔으니 0%로 시작 — 실제 값 갱신은 fsm_ota_event_callback()의
 * OTA_CLIENT_EVENT_PROGRESS 케이스에서. */
static void on_enter_ota_receiving(void)
{
    display_ui_set_status_scroll("RX 0%");
    /* TODO(팀2): OTA 수신 버퍼 초기화, 음성 태스크 일시 중단 */
}

/* OTA_COMPLETE(모든 청크 수신 완료)로 여기 진입 — 실제 이미지 검증/파티션
 * 기록은 팀2 TODO로 남겨두고, 지금은 진행 중임을 영어로 표시만 한다. */
static void on_enter_ota_applying(void)
{
    display_ui_set_status_scroll("APPLY PENDING");
    /* TODO(팀2): 이미지 검증 및 OTA 파티션 기록 */
}
/*
 * EV_ERROR는 전역 전이라(fsm_task() 참고) 어느 상태에서든 여기로 곧장 올 수
 * 있다 — TX_AUDIO/RX_AUDIO 도중이었을 수도 있어서, on_enter_menu_comm()과
 * 동일하게 그 태스크들부터 정리해야 "안전 상태로 정지"가 실제로 보장된다.
 * LED 빨간 점멸(status_led_start_error_blink(), PTT 흰색 고정과 구분되는
 * 패턴)과 OLED 상태 줄("ERROR")로 눈에 보이게 표시. ERROR의 유일한 탈출구인
 * EV_RETRY -> BOOT_INIT에서 둘 다 정리된다(on_enter_boot_init() 참고).
 */
static void on_enter_error(void)
{
    ESP_LOGE(TAG, "entering ERROR state");

    stop_tx_audio_task();
    /* ERROR에서도 RF TX 세션을 반드시 닫아 producer가 멈춘 뒤 큐가 차며
     * SUBMIT_FAIL이 반복되는 현상을 막고, 상대 수신 가능한 시작 상태로 복귀한다. */
    if (!fhss_audio_adapter_end_tx()) {
        ESP_LOGW(TAG, "failed to stop FHSS audio TX while entering ERROR");
    }
    stop_rx_audio_task();

    status_led_start_error_blink();
    display_ui_set_status("ERROR");
}

static void (*const s_enter_actions[FSM_STATE_COUNT])(void) = {
    [FSM_STATE_BOOT_INIT]     = on_enter_boot_init,
    [FSM_STATE_MENU_COMM]     = on_enter_menu_comm,
    [FSM_STATE_MENU_IDLE]     = on_enter_menu_idle,
    [FSM_STATE_MENU_OTA]      = on_enter_menu_ota,
    [FSM_STATE_TX_AUDIO]      = on_enter_tx_audio,
    [FSM_STATE_RX_AUDIO]      = on_enter_rx_audio,
    [FSM_STATE_OTA_RECEIVING] = on_enter_ota_receiving,
    [FSM_STATE_OTA_APPLYING]  = on_enter_ota_applying,
    [FSM_STATE_ERROR]         = on_enter_error,
};

static void fsm_transition_to(fsm_state_t next_state)
{
    if (next_state == s_state) {
        return;
    }
    ESP_LOGI(TAG, "%s -> %s", s_state_names[s_state], s_state_names[next_state]);
    s_state = next_state;
    if (s_enter_actions[s_state] != NULL) {
        s_enter_actions[s_state]();
    }
}

static void fsm_task(void *arg)
{
    fsm_event_t event;

    /* 부팅 직후 진입 동작 1회 실행 */
    if (s_enter_actions[s_state] != NULL) {
        s_enter_actions[s_state]();
    }

    for (;;) {
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGD(TAG, "event %s in state %s", s_event_names[event], s_state_names[s_state]);

        /* 전역 전이: 어느 상태에서든 적용 */
        if (event == FSM_EVENT_ERROR && s_state != FSM_STATE_ERROR) {
            fsm_transition_to(FSM_STATE_ERROR);
            continue;
        }
        if (event == FSM_EVENT_SYNC_LOST &&
            s_state != FSM_STATE_BOOT_INIT &&
            s_state != FSM_STATE_ERROR) {
            fsm_transition_to(FSM_STATE_MENU_COMM);
            continue;
        }

        /* 상태별 전이표 조회 */
        bool handled = false;
        for (size_t i = 0; i < sizeof(s_transitions) / sizeof(s_transitions[0]); i++) {
            if (s_transitions[i].state == s_state && s_transitions[i].event == event) {
                fsm_transition_to(s_transitions[i].next_state);
                handled = true;
                break;
            }
        }
        if (!handled) {
            ESP_LOGW(TAG, "unhandled event %s in state %s", s_event_names[event], s_state_names[s_state]);
        }
    }
}

void fsm_init(void)
{
    s_event_queue = xQueueCreate(16, sizeof(fsm_event_t));
    s_rx_audio_queue = xQueueCreate(FSM_RX_AUDIO_QUEUE_DEPTH, sizeof(fsm_rx_audio_frame_t));
    xTaskCreate(fsm_task, "fsm_task", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
}

void fsm_post_event(fsm_event_t event)
{
    xQueueSend(s_event_queue, &event, 0);
}

bool fsm_post_rx_audio_frame(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > FSM_RX_AUDIO_FRAME_MAX_BYTES) {
        return false;
    }

    fsm_rx_audio_frame_t frame;
    frame.len = len;
    memcpy(frame.data, data, len);

    if (xQueueSend(s_rx_audio_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx audio queue full, dropping frame");
        return false;
    }

    fsm_post_event(FSM_EVENT_RX_FRAME);
    return true;
}

fsm_state_t fsm_get_state(void)
{
    return s_state;
}

const char *fsm_state_name(fsm_state_t state)
{
    return (state < FSM_STATE_COUNT) ? s_state_names[state] : "UNKNOWN";
}

const char *fsm_event_name(fsm_event_t event)
{
    return (event < FSM_EVENT_COUNT) ? s_event_names[event] : "UNKNOWN";
}

bool fsm_ota_mode_callback(void *context)
{
    (void)context;
    return fsm_get_state() == FSM_STATE_MENU_OTA;
}

void fsm_ota_event_callback(
    ota_client_event_t event,
    uint32_t progress_percent,
    esp_err_t error,
    void *context
)
{
    (void)context;

    switch (event) {
        case OTA_CLIENT_EVENT_STARTED:
            fsm_post_event(FSM_EVENT_OTA_START);
            break;
        case OTA_CLIENT_EVENT_PROGRESS: {
            ESP_LOGI(TAG, "OTA progress: %" PRIu32 "%%", progress_percent);
            /* 실제 청크 개수(n/N)를 표시하려면 ota_client가 progress_percent
             * 말고 총/수신 청크 수도 넘겨줘야 하는데, 지금 콜백 시그니처엔
             * 없어서(팀2 컴포넌트라 임의로 API를 안 늘림) percent로만 표시.
             * 화면 폭 제약도 없어서 진행바 대신 텍스트로 충분(사용자 확인). */
            char status_buf[16];
            snprintf(status_buf, sizeof(status_buf), "RX %" PRIu32 "%%", progress_percent);
            display_ui_set_status_scroll(status_buf);
            break;
        }
        case OTA_CLIENT_EVENT_APPLYING:
            fsm_post_event(FSM_EVENT_OTA_COMPLETE);
            break;
        case OTA_CLIENT_EVENT_COMPLETED:
            fsm_post_event(FSM_EVENT_OTA_VERIFY_OK);
            break;
        case OTA_CLIENT_EVENT_FAILED:
            ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(error));
            if (fsm_get_state() == FSM_STATE_OTA_APPLYING) {
                /* MENU_OTA로 조용히 돌아가기 전에 실패했다는 걸 3초간
                 * 보여준다. FSM_EVENT_OTA_VERIFY_FAIL을 먼저 올려버리면
                 * on_enter_menu_ota()가 곧바로 STANDBY로 덮어써서 실패
                 * 사실이 화면에 전혀 안 남으므로, 메시지를 다 보여준 뒤에
                 * 이벤트를 올리는 순서로 함. 여기서 블로킹되는 건 이
                 * 콜백을 부른 ota_client 컨슈머 태스크지 fsm_task가
                 * 아니라서(다른 이벤트 처리와는 무관), 3초 정도는
                 * 문제없다고 판단(on_enter_tx_audio()의 삐빅음 블로킹과
                 * 같은 이유로 "메시지 다 보여준 뒤 다음 전이"를 보장). */
                display_ui_set_status_scroll("OTA FAILED");
                vTaskDelay(pdMS_TO_TICKS(3000));
                fsm_post_event(FSM_EVENT_OTA_VERIFY_FAIL);
            } else {
                fsm_post_event(FSM_EVENT_ERROR);
            }
            break;
        case OTA_CLIENT_EVENT_ABORTED:
            ESP_LOGW(TAG, "OTA aborted at %" PRIu32 "%%", progress_percent);
            break;
        default:
            break;
    }
}
