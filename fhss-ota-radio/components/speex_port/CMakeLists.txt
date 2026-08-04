# components/speex(서브모듈, xiph/speex 원본, pristine 유지)를
# ESP-IDF 컴포넌트로 빌드하기 위한 래퍼.
#
# 서브모듈 안에는 이 CMakeLists.txt를 두지 않는다 — 서브모듈 워킹트리에 파일을
# 추가해도 부모 레포는 그 파일을 추적하지 못하므로(서브모듈은 커밋 SHA만 기록),
# 재클론 시 사라진다. 그래서 소스는 상대경로로 서브모듈을 가리키기만 한다.
#
# 협대역(narrowband, 8kHz)만 포함한다 — wideband/ultra-wideband(sb_celp,
# modes_wb), stereo, FFT 기반 vorbis_psy(VORBIS_PSYCHO 미정의라 불필요),
# Ogg 스타일 헤더(speex_header, 우리는 프레임을 직접 RF로 실어 나르므로 불필요)는
# 제외했다. 필요해지면 이 목록에 추가하면 된다.

set(SPEEX_DIR "${CMAKE_CURRENT_LIST_DIR}/../speex")

idf_component_register(
    SRCS
        "${SPEEX_DIR}/libspeex/bits.c"
        "${SPEEX_DIR}/libspeex/cb_search.c"
        "${SPEEX_DIR}/libspeex/exc_10_16_table.c"
        "${SPEEX_DIR}/libspeex/exc_10_32_table.c"
        "${SPEEX_DIR}/libspeex/exc_20_32_table.c"
        "${SPEEX_DIR}/libspeex/exc_5_256_table.c"
        "${SPEEX_DIR}/libspeex/exc_5_64_table.c"
        "${SPEEX_DIR}/libspeex/exc_8_128_table.c"
        "${SPEEX_DIR}/libspeex/filters.c"
        "${SPEEX_DIR}/libspeex/gain_table.c"
        "${SPEEX_DIR}/libspeex/gain_table_lbr.c"
        "${SPEEX_DIR}/libspeex/high_lsp_tables.c"
        "${SPEEX_DIR}/libspeex/lpc.c"
        "${SPEEX_DIR}/libspeex/lsp.c"
        "${SPEEX_DIR}/libspeex/lsp_tables_nb.c"
        "${SPEEX_DIR}/libspeex/ltp.c"
        "${SPEEX_DIR}/libspeex/modes.c"
        "${SPEEX_DIR}/libspeex/nb_celp.c"
        "${SPEEX_DIR}/libspeex/quant_lsp.c"
        "${SPEEX_DIR}/libspeex/speex.c"
        "${SPEEX_DIR}/libspeex/speex_callbacks.c"
        "${SPEEX_DIR}/libspeex/vbr.c"
        "${SPEEX_DIR}/libspeex/vq.c"
        "${SPEEX_DIR}/libspeex/window.c"
    INCLUDE_DIRS
        "${SPEEX_DIR}/include"    # 공개 헤더: speex/speex.h, speex/speex_bits.h ...
        "generated"               # speex_config_types.h (autotools 생성물 대체)
    PRIV_INCLUDE_DIRS
        "${SPEEX_DIR}/libspeex"   # 내부 전용 헤더: arch.h, os_support.h ...
)

# FLOATING_POINT: ESP32-S3는 단정도 FPU가 있어 고정소수점보다 float 경로가 단순/충분.
# EXPORT: autotools가 만드는 config.h에서 보통 빈 매크로로 정의되는 것을 여기서 대신 정의.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FLOATING_POINT
    EXPORT=
)

# 서드파티 코드라 경고를 프로젝트 전체 -Werror 정책에 종속시키지 않는다.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-but-set-variable
    -Wno-unused-variable
)
