/*
 * xiph/speex는 autotools/meson이 include/speex/speex_config_types.h.in을
 * 실제 타입으로 치환해 생성한다. 이 프로젝트는 그 빌드 스텝을 쓰지 않으므로,
 * 서브모듈(components/speex, pristine 상태 유지) 대신 여기서 직접 생성 결과를
 * 제공한다. speex_types.h가 "speex_config_types.h"로 찾는 파일이 바로 이것이다.
 */
#ifndef __SPEEX_TYPES_H__
#define __SPEEX_TYPES_H__

#include <stdint.h>

typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;

#endif
