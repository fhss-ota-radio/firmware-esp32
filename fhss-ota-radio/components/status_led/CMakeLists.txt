# status_led 컴포넌트: 온보드 WS2812 RGB LED(GPIO38, RMT 기반) 상태 표시.
#
# led_strip은 Espressif 공식 managed component다 — 이 폴더의 idf_component.yml로
# 의존성만 선언해두면, 빌드 시 ESP-IDF 컴포넌트 매니저가 자동으로 받아와
# managed_components/에 캐시한다(레포에는 커밋 안 됨, .gitignore 처리).

idf_component_register(
    SRCS "status_led.c"
    INCLUDE_DIRS "."
    REQUIRES led_strip esp_timer
)
