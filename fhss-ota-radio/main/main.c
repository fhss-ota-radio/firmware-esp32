#include <stdio.h>

#include "fsm.h"

void app_main(void)
{
    fsm_init();

    /* TODO: 실제 주변장치 초기화(I2S/OLED/SPI/GPIO)가 끝난 뒤 아래 이벤트를 발생시킨다. */
    fsm_post_event(FSM_EVENT_INIT_DONE);

#if CONFIG_OTA_LOCAL_TEST
    ota_test_start();
#endif

}