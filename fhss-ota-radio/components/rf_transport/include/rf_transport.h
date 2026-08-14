#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    RF_TRANSPORT_STATUS_OK = 0,
    RF_TRANSPORT_STATUS_INVALID_ARG,
    RF_TRANSPORT_STATUS_NOT_INITIALIZED,
    RF_TRANSPORT_STATUS_SPI_ERROR,
    RF_TRANSPORT_STATUS_TIMEOUT,
} rf_transport_status_t;


/*
 * ESP32-S3 ↔ CC1101 SPI 설정
 *
 * GPIO 번호를 코드에 고정하지 않고
 * 외부에서 전달하도록 구성한다.
 */
typedef struct {
    spi_host_device_t spi_host;

    gpio_num_t sclk_gpio;
    gpio_num_t mosi_gpio;
    gpio_num_t miso_gpio;
    gpio_num_t cs_gpio;
    gpio_num_t gdo0_gpio;

    int spi_clock_hz;
    bool enable_gdo0_interrupt;
} rf_transport_config_t;


/*
 * CC1101 식별 정보
 */
typedef struct {
    uint8_t partnum;
    uint8_t version;
} rf_transport_chip_info_t;

#define RF_TRANSPORT_MAX_PACKET_LENGTH 60U

typedef struct {
    uint8_t payload[RF_TRANSPORT_MAX_PACKET_LENGTH];
    uint8_t length;
    int16_t rssi_dbm;
    uint8_t lqi;
    bool crc_ok;
} rf_transport_rx_packet_t;


/*
 * rf_transport 내부 상태
 */
typedef struct {
    spi_device_handle_t spi_device;

    spi_host_device_t spi_host;
    gpio_num_t cs_gpio;
    gpio_num_t miso_gpio;
    gpio_num_t gdo0_gpio;

    void *rx_timestamp_queue;

    bool initialized;
    bool gdo0_interrupt_enabled;
} rf_transport_t;


/*
 * SPI bus와 CC1101 device 초기화
 */
rf_transport_status_t rf_transport_init(
    rf_transport_t *transport,
    const rf_transport_config_t *config
);


/*
 * CC1101 PARTNUM / VERSION 읽기
 *
 * 첫 하드웨어 연결 확인용 진단 API
 */
rf_transport_status_t rf_transport_read_chip_info(
    const rf_transport_t *transport,
    rf_transport_chip_info_t *out_info
);

/* 433.92 MHz, 38.4 kBaud, 2-FSK, variable-length packet, CRC enabled. */
rf_transport_status_t rf_transport_configure_433mhz(
    const rf_transport_t *transport
);

/* Recovers the SPI framing after a service task is stopped during an RF
 * operation, then resets and reapplies the 433 MHz CC1101 configuration. */
rf_transport_status_t rf_transport_recover_433mhz(
    const rf_transport_t *transport
);

/* Selects a CC1101 CHANNR value. The radio is returned to IDLE first. */
rf_transport_status_t rf_transport_set_channel(
    const rf_transport_t *transport,
    uint8_t channel
);

/* Enters RX state so GDO0 can signal sync-word detection. */
rf_transport_status_t rf_transport_start_receive(
    const rf_transport_t *transport
);

rf_transport_status_t rf_transport_send_packet(
    const rf_transport_t *transport,
    const uint8_t *payload,
    uint8_t length
);

rf_transport_status_t rf_transport_receive_packet(
    const rf_transport_t *transport,
    uint32_t timeout_ms,
    rf_transport_rx_packet_t *out_packet
);

/* Waits for a GDO0 rising edge and returns its esp_timer timestamp. */
rf_transport_status_t rf_transport_wait_rx_timestamp(
    const rf_transport_t *transport,
    uint32_t timeout_ms,
    int64_t *out_timestamp_us
);


#ifdef __cplusplus
}
#endif
