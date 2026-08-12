#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_id_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 이 기기의 고유 식별자를 out[DEVICE_ID_LEN]에 채운다. ESP32-S3 eFuse에
 * 공장에서 구워진 base MAC 주소 뒤 DEVICE_ID_LEN(3)바이트를 그대로 쓴다
 * (device_id_config.h 주석 참고). Wi-Fi/BT 스택 초기화 여부와 무관하게
 * eFuse만 읽으므로 부팅 후 언제든 호출 가능하고, 펌웨어를 재플래시해도
 * 값이 바뀌지 않는다(eFuse는 OTP라 이 값은 칩 자체에 고정됨) — 그래서
 * 매 펌웨어 패치마다 기기별로 값을 따로 관리/수정할 필요가 없다.
 */
void device_id_get(uint8_t out[DEVICE_ID_LEN]);

/*
 * device_id_get()의 DEVICE_ID_LEN바이트를 "AABBCC" 형식의 대문자 hex
 * 문자열로 채운다(널 종료 포함, out_capacity는 DEVICE_ID_LEN*2+1 이상
 * 필요). 로그 태그/OLED 표시 등 사람이 읽는 용도.
 */
void device_id_get_hex(char *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif
