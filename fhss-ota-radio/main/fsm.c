#include "fsm.h"

#include <stdbool.h>
#include <inttypes.h>
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
#include "ptt_button.h"
#include "rotary_encoder.h"
#include "status_led.h"

static const char *TAG = "fsm";

static QueueHandle_t s_event_queue;
static fsm_state_t s_state = FSM_STATE_BOOT_INIT;

/*
 * 오디오 프레임 전용 큐. fsm_event_t(이벤트 큐)는 페이로드가 없는 enum이라
 * 데이터를 못 실으므로, 수신 프레임 바이트는 이 큐로 따로 옮기고 이벤트
 * 큐에는 "도착했다"는 신호(FSM_EVENT_RX_FRAME)만 올린다. 큐 깊이 4개 =
 * 20ms/프레임 기준 약 80ms 지터 버퍼.
 */
#define FSM_RX_AUDIO_FRAME_MAX_BYTES AUDIO_CODEC_MAX_ENCODED_BYTES
#define FSM_RX_AUDIO_QUEUE_DEPTH     4

/* 이 시간 동안 새 프레임이 안 오면 수신이 끝난 것으로 보고 FSM_EVENT_RX_DONE을
 * 스스로 올린다 (무음/타임아웃 기반 종료 — 결정 근거는 docs/fsm-design.md 참고). */
#define FSM_RX_AUDIO_IDLE_TIMEOUT_MS 1000

typedef struct {
    uint8_t data[FSM_RX_AUDIO_FRAME_MAX_BYTES];
    size_t len;
} fsm_rx_audio_frame_t;

static QueueHandle_t s_rx_audio_queue;

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
    /* status_led는 FSM 처리 결과를 기다리지 않고 PTT 원시 입력을 그대로
     * 반영한다 — GPIO 디바운스만 통과하면 바로 켜지는 테스트용 표시라,
     * FSM 전이표가 어떻게 바뀌든(지금은 MENU_COMM에서 TX_AUDIO로 실제 전이됨)
     * 영향받지 않는다. */
    if (pressed) {
        status_led_set_white_dim();
    } else {
        status_led_off();
    }

#ifdef LOOPBACK_ENABLE
    /* TEMP: MENU_IDLE에서는 정식 FSM 이벤트 대신 마이크 loopback 테스트로 라우팅 */
    if (fsm_get_state() == FSM_STATE_MENU_IDLE) {
        if (pressed) {
            if (s_mic_test_task == NULL) {
                s_mic_test_recording = true;
                xTaskCreate(mic_test_task, "mic_test", 8192, NULL, tskIDLE_PRIORITY + 3, &s_mic_test_task);
            }
        } else {
            s_mic_test_recording = false;
        }
        return;
    }
#endif /* LOOPBACK_ENABLE */

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

static void tx_audio_task(void *arg)
{
    uint8_t frame[AUDIO_CODEC_MAX_ENCODED_BYTES];

    for (;;) {
        int n = audio_io_capture_encode(frame, sizeof(frame));
        if (n > 0) {
            /* TODO(팀5): frame[0..n)을 rf_transport로 FHSS 채널 송신 */
        }
    }
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

static void rx_audio_task(void *arg)
{
    fsm_rx_audio_frame_t frame;

    for (;;) {
        if (xQueueReceive(s_rx_audio_queue, &frame, pdMS_TO_TICKS(FSM_RX_AUDIO_IDLE_TIMEOUT_MS)) == pdTRUE) {
            audio_io_decode_play(frame.data, frame.len);
        } else {
            break;
        }
    }

    s_rx_audio_task = NULL;
    fsm_post_event(FSM_EVENT_RX_DONE);
    vTaskDelete(NULL);
}

/* 상태별 진입 동작. 실제 하드웨어 제어는 각 담당(TODO)이 채운다. */
static void on_enter_boot_init(void)
{
    char id_hex[DEVICE_ID_LEN * 2 + 1];
    device_id_get_hex(id_hex, sizeof(id_hex));
    ESP_LOGI(TAG, "device id: %s", id_hex);

    display_ui_init();
    status_led_init();

    ptt_button_init();
    ptt_button_set_callback(on_ptt_event, NULL);

    rotary_encoder_init();
    rotary_encoder_set_select_callback(on_menu_select, NULL);
    rotary_encoder_set_cursor_callback(on_menu_cursor, NULL);

    audio_codec_init();
    audio_io_init();

    /* TODO(팀2): SPI(rf_transport/CC1101) 초기화 — 해당 컴포넌트 생기면 추가 */
}
/* MENU_COMM: 통신 대기(기본 메뉴). TX_AUDIO/RX_AUDIO는 여기서만 나가고
 * 여기로만 돌아오니, 오디오 태스크 정리는 전부 여기서 한다(이전엔
 * on_enter_menu_idle이라는 이름이었음 — 2026-08-11에 개명, 새 MENU_IDLE은
 * 완전히 다른 뮤트 상태). */
static void on_enter_menu_comm(void)
{
    if (s_tx_audio_task != NULL) {
        vTaskDelete(s_tx_audio_task);
        s_tx_audio_task = NULL;
    }
    if (s_rx_audio_task != NULL) {
        vTaskDelete(s_rx_audio_task);
        s_rx_audio_task = NULL;
        audio_io_speaker_disable();
    }
    display_ui_draw_menu(DISPLAY_UI_MENU_COMM, menu_item_from_rotary(rotary_encoder_get_cursor()));
    display_ui_set_status("HOLD PTT");
}

/* MENU_IDLE(뮤트): TX_AUDIO/RX_AUDIO로 들어오는 전이가 없어서(전이표 참고)
 * 오디오 태스크가 실행 중일 수 없다 — 정리할 게 없다. */
static void on_enter_menu_idle(void)
{
    display_ui_draw_menu(DISPLAY_UI_MENU_IDLE, menu_item_from_rotary(rotary_encoder_get_cursor()));
#ifdef LOOPBACK_ENABLE
    /* "PTT 눌러서 마이크/스피커 테스트" 안내 — 8자 한도라 최대한 축약
     * (원문 의도: test mic and speaker via pressing ptt button). */
    display_ui_set_status("PTT:TEST");
#else
    display_ui_set_status("MUTED");
#endif
}

static void on_enter_menu_ota(void)
{
    display_ui_draw_menu(DISPLAY_UI_MENU_OTA, menu_item_from_rotary(rotary_encoder_get_cursor()));
    display_ui_set_status_animated("WAIT");
    /* TODO(팀2): CC1101 OTA 채널 리스닝 준비. OTA_RECEIVING 진행률 표시는
     * 상태 영역에 여유(STATUS_H, display_ui.c 참고)를 남겨뒀으니 그때 추가. */
}
static void on_enter_tx_audio(void)
{
    display_ui_set_status_animated("TX");

    /* fsm_task 안에서 블로킹으로 끝까지 재생 — 이유는 tx_audio_task 주석 참고. */
    audio_io_speaker_enable();
    audio_io_play_beep();
    audio_io_speaker_disable();

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

    /* audio_codec_decode()도 같은 호출 체인 무게라 tx와 동일하게 8192로. */
    audio_io_speaker_enable();
    xTaskCreate(rx_audio_task, "rx_audio", 8192, NULL, tskIDLE_PRIORITY + 3, &s_rx_audio_task);
}
static void on_enter_ota_receiving(void) { /* TODO(팀2): OTA 수신 버퍼 초기화, 음성 태스크 일시 중단 */ }
static void on_enter_ota_applying(void)  { /* TODO(팀2): 이미지 검증 및 OTA 파티션 기록 */ }
static void on_enter_error(void)         { /* TODO(팀1/PM): 오류 로깅, 안전 상태로 정지 */ }

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
        case OTA_CLIENT_EVENT_PROGRESS:
            ESP_LOGI(TAG, "OTA progress: %" PRIu32 "%%", progress_percent);
            break;
        case OTA_CLIENT_EVENT_APPLYING:
            fsm_post_event(FSM_EVENT_OTA_COMPLETE);
            break;
        case OTA_CLIENT_EVENT_COMPLETED:
            fsm_post_event(FSM_EVENT_OTA_VERIFY_OK);
            break;
        case OTA_CLIENT_EVENT_FAILED:
            ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(error));
            if (fsm_get_state() == FSM_STATE_OTA_APPLYING) {
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
