#include "rf_transport.h"

#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define CC1101_PARTNUM_ADDR       0x30U
#define CC1101_VERSION_ADDR       0x31U
#define CC1101_READ_BURST         0xC0U
#define CC1101_READY_TIMEOUT_US   10000LL
#define CC1101_PACKET_TIMEOUT_US  100000LL
#define CC1101_RESET_DELAY_US     40U

#define CC1101_WRITE_BURST        0x40U
#define CC1101_READ_SINGLE        0x80U
#define CC1101_FIFO_ADDR          0x3FU
#define CC1101_PATABLE_ADDR       0x3EU
#define CC1101_CHANNR_ADDR        0x0AU

#define CC1101_SRES               0x30U
#define CC1101_SRX                0x34U
#define CC1101_STX                0x35U
#define CC1101_SIDLE              0x36U
#define CC1101_SFRX               0x3AU
#define CC1101_SFTX               0x3BU

#define CC1101_MARCSTATE_ADDR     0x35U
#define CC1101_TXBYTES_ADDR       0x3AU
#define CC1101_RXBYTES_ADDR       0x3BU
#define CC1101_MARCSTATE_IDLE     0x01U
#define CC1101_MARCSTATE_RX       0x0DU
#define CC1101_MARCSTATE_MASK     0x1FU
#define CC1101_FIFO_ERROR_MASK    0x80U
#define CC1101_FIFO_COUNT_MASK    0x7FU

static const char *TAG = "rf_transport";

/* 송신 실패는 상위 계층에서 단순 ERROR로 합쳐지므로, 실제 CC1101 단계와
 * 상태 레지스터 값은 하드웨어 경계인 이 파일에서 오류가 날 때만 기록한다. */
static const char *status_name(rf_transport_status_t status)
{
    switch (status) {
    case RF_TRANSPORT_STATUS_OK: return "OK";
    case RF_TRANSPORT_STATUS_INVALID_ARG: return "INVALID_ARG";
    case RF_TRANSPORT_STATUS_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case RF_TRANSPORT_STATUS_SPI_ERROR: return "SPI_ERROR";
    case RF_TRANSPORT_STATUS_TIMEOUT: return "TIMEOUT";
    default: return "UNKNOWN";
    }
}

typedef struct {
    uint8_t address;
    uint8_t value;
} cc1101_register_setting_t;

static void cc1101_gdo0_isr(void *arg)
{
    rf_transport_t *transport = (rf_transport_t *)arg;
    if (transport == NULL || transport->rx_timestamp_queue == NULL) {
        return;
    }

    const int64_t timestamp_us = esp_timer_get_time();
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueOverwriteFromISR(
        (QueueHandle_t)transport->rx_timestamp_queue,
        &timestamp_us,
        &higher_priority_task_woken
    );
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/* SmartRF-style 38.4 kBaud sensitivity settings, frequency changed to 433.92 MHz. */
static const cc1101_register_setting_t s_433mhz_settings[] = {
    {0x00U, 0x29U}, {0x02U, 0x06U}, {0x03U, 0x47U},
    {0x04U, 0xD3U}, {0x05U, 0x91U}, {0x06U, RF_TRANSPORT_MAX_PACKET_LENGTH},
    {0x07U, 0x04U}, {0x08U, 0x05U}, {0x09U, 0x00U}, {0x0AU, 0x00U},
    {0x0BU, 0x06U}, {0x0CU, 0x00U},
    {0x0DU, 0x10U}, {0x0EU, 0xB0U}, {0x0FU, 0x71U},
    {0x10U, 0xCAU}, {0x11U, 0x83U}, {0x12U, 0x03U},
    {0x13U, 0x22U}, {0x14U, 0xF8U}, {0x15U, 0x35U},
    {0x16U, 0x07U}, {0x17U, 0x00U}, {0x18U, 0x18U},
    {0x19U, 0x16U}, {0x1AU, 0x6CU},
    {0x1BU, 0x43U}, {0x1CU, 0x40U}, {0x1DU, 0x91U},
    {0x21U, 0x56U}, {0x22U, 0x10U},
    {0x23U, 0xE9U}, {0x24U, 0x2AU}, {0x25U, 0x00U}, {0x26U, 0x1FU},
    {0x2CU, 0x81U}, {0x2DU, 0x35U}, {0x2EU, 0x09U},
};

static rf_transport_status_t wait_until_ready(
    const rf_transport_t *transport
)
{
    const int64_t deadline_us =
        esp_timer_get_time() + CC1101_READY_TIMEOUT_US;

    while (gpio_get_level(transport->miso_gpio) != 0) {
        if (esp_timer_get_time() >= deadline_us) {
            return RF_TRANSPORT_STATUS_TIMEOUT;
        }
    }

    return RF_TRANSPORT_STATUS_OK;
}

static rf_transport_status_t spi_transfer(
    const rf_transport_t *transport,
    const uint8_t *tx_data,
    uint8_t *rx_data,
    size_t length
)
{
    if (transport == NULL || tx_data == NULL || length == 0U) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (!transport->initialized) {
        return RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }

    spi_transaction_t transaction = {
        .length = length * 8U,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    gpio_set_level(transport->cs_gpio, 0);
    const rf_transport_status_t ready_status = wait_until_ready(transport);
    if (ready_status != RF_TRANSPORT_STATUS_OK) {
        gpio_set_level(transport->cs_gpio, 1);
        return ready_status;
    }

    const esp_err_t spi_status =
        spi_device_polling_transmit(transport->spi_device, &transaction);
    gpio_set_level(transport->cs_gpio, 1);

    return spi_status == ESP_OK
        ? RF_TRANSPORT_STATUS_OK
        : RF_TRANSPORT_STATUS_SPI_ERROR;
}

static rf_transport_status_t command_strobe(
    const rf_transport_t *transport,
    uint8_t command
)
{
    return spi_transfer(transport, &command, NULL, 1U);
}

static rf_transport_status_t reset_radio(
    const rf_transport_t *transport
)
{
    if (transport == NULL || !transport->initialized) {
        return transport == NULL
            ? RF_TRANSPORT_STATUS_INVALID_ARG
            : RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }

    /* CC1101 데이터시트의 수동 power-on reset 순서를 그대로 수행한다.
     * 첫 CS LOW에서 SO가 LOW가 된 것을 확인한 뒤 CS를 다시 HIGH로 올리고
     * 40 us 기다려야 SPI 상태기가 다음 SRES를 확실히 command로 인식한다. */
    gpio_set_level(transport->cs_gpio, 1);
    esp_rom_delay_us(CC1101_RESET_DELAY_US);

    gpio_set_level(transport->cs_gpio, 0);
    rf_transport_status_t status = wait_until_ready(transport);
    gpio_set_level(transport->cs_gpio, 1);
    if (status != RF_TRANSPORT_STATUS_OK) {
        return status;
    }

    esp_rom_delay_us(CC1101_RESET_DELAY_US);
    gpio_set_level(transport->cs_gpio, 0);
    status = wait_until_ready(transport);
    if (status != RF_TRANSPORT_STATUS_OK) {
        gpio_set_level(transport->cs_gpio, 1);
        return status;
    }

    const uint8_t command = CC1101_SRES;
    spi_transaction_t transaction = {
        .length = 8U,
        .tx_buffer = &command,
    };
    status = spi_device_polling_transmit(
                 transport->spi_device,
                 &transaction) == ESP_OK
        ? RF_TRANSPORT_STATUS_OK
        : RF_TRANSPORT_STATUS_SPI_ERROR;
    if (status == RF_TRANSPORT_STATUS_OK) {
        status = wait_until_ready(transport);
    }
    gpio_set_level(transport->cs_gpio, 1);
    return status;
}

static rf_transport_status_t write_register(
    const rf_transport_t *transport,
    uint8_t address,
    uint8_t value
)
{
    const uint8_t tx_data[2] = {address, value};
    return spi_transfer(transport, tx_data, NULL, sizeof(tx_data));
}

static rf_transport_status_t write_burst(
    const rf_transport_t *transport,
    uint8_t address,
    const uint8_t *data,
    size_t length
)
{
    if (data == NULL || length == 0U || length > RF_TRANSPORT_MAX_PACKET_LENGTH + 1U) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }

    uint8_t tx_data[RF_TRANSPORT_MAX_PACKET_LENGTH + 2U] = {0};
    tx_data[0] = (uint8_t)(CC1101_WRITE_BURST | address);
    memcpy(&tx_data[1], data, length);
    return spi_transfer(transport, tx_data, NULL, length + 1U);
}

static rf_transport_status_t read_burst(
    const rf_transport_t *transport,
    uint8_t address,
    uint8_t *data,
    size_t length
)
{
    if (data == NULL || length == 0U || length > RF_TRANSPORT_MAX_PACKET_LENGTH + 2U) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }

    uint8_t tx_data[RF_TRANSPORT_MAX_PACKET_LENGTH + 3U] = {0};
    uint8_t rx_data[RF_TRANSPORT_MAX_PACKET_LENGTH + 3U] = {0};
    tx_data[0] = (uint8_t)(CC1101_READ_BURST | address);

    const rf_transport_status_t status =
        spi_transfer(transport, tx_data, rx_data, length + 1U);
    if (status != RF_TRANSPORT_STATUS_OK) {
        return status;
    }

    memcpy(data, &rx_data[1], length);
    return RF_TRANSPORT_STATUS_OK;
}

static rf_transport_status_t read_status_register(
    const rf_transport_t *transport,
    uint8_t address,
    uint8_t *out_value
)
{
    if (transport == NULL || out_value == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (!transport->initialized) {
        return RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }

    const uint8_t tx_data[2] = {
        (uint8_t)(CC1101_READ_BURST | address),
        0U,
    };
    uint8_t rx_data[2] = {0};

    const rf_transport_status_t status =
        spi_transfer(transport, tx_data, rx_data, sizeof(tx_data));
    if (status != RF_TRANSPORT_STATUS_OK) {
        return status;
    }

    *out_value = rx_data[1];
    return RF_TRANSPORT_STATUS_OK;
}

static rf_transport_status_t read_config_register(
    const rf_transport_t *transport,
    uint8_t address,
    uint8_t *out_value
)
{
    if (transport == NULL || out_value == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }

    const uint8_t tx_data[2] = {
        (uint8_t)(CC1101_READ_SINGLE | address),
        0U,
    };
    uint8_t rx_data[2] = {0};
    const rf_transport_status_t status =
        spi_transfer(transport, tx_data, rx_data, sizeof(tx_data));
    if (status == RF_TRANSPORT_STATUS_OK) {
        *out_value = rx_data[1];
    }
    return status;
}

rf_transport_status_t rf_transport_init(
    rf_transport_t *transport,
    const rf_transport_config_t *config
)
{
    if (transport == NULL || config == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (config->sclk_gpio < 0 ||
        config->mosi_gpio < 0 ||
        config->miso_gpio < 0 ||
        config->cs_gpio < 0 ||
        config->spi_clock_hz <= 0) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (config->enable_gdo0_interrupt && config->gdo0_gpio < 0) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }

    QueueHandle_t rx_timestamp_queue = NULL;
    if (config->enable_gdo0_interrupt) {
        rx_timestamp_queue = xQueueCreate(1U, sizeof(int64_t));
        if (rx_timestamp_queue == NULL) {
            return RF_TRANSPORT_STATUS_SPI_ERROR;
        }
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = config->miso_gpio,
        .sclk_io_num = config->sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RF_TRANSPORT_MAX_PACKET_LENGTH + 3U,
    };

    esp_err_t status = spi_bus_initialize(
        config->spi_host,
        &bus_config,
        SPI_DMA_CH_AUTO
    );
    if (status != ESP_OK) {
        if (rx_timestamp_queue != NULL) {
            vQueueDelete(rx_timestamp_queue);
        }
        return RF_TRANSPORT_STATUS_SPI_ERROR;
    }

    const gpio_config_t cs_config = {
        .pin_bit_mask = 1ULL << config->cs_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    status = gpio_config(&cs_config);
    if (status != ESP_OK) {
        spi_bus_free(config->spi_host);
        if (rx_timestamp_queue != NULL) {
            vQueueDelete(rx_timestamp_queue);
        }
        return RF_TRANSPORT_STATUS_SPI_ERROR;
    }
    gpio_set_level(config->cs_gpio, 1);

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = config->spi_clock_hz,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };

    spi_device_handle_t spi_device = NULL;
    status = spi_bus_add_device(
        config->spi_host,
        &device_config,
        &spi_device
    );
    if (status != ESP_OK) {
        spi_bus_free(config->spi_host);
        if (rx_timestamp_queue != NULL) {
            vQueueDelete(rx_timestamp_queue);
        }
        return RF_TRANSPORT_STATUS_SPI_ERROR;
    }

    if (config->enable_gdo0_interrupt) {
        const gpio_config_t gdo0_config = {
            .pin_bit_mask = 1ULL << config->gdo0_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_POSEDGE,
        };
        status = gpio_config(&gdo0_config);
        if (status != ESP_OK) {
            spi_bus_remove_device(spi_device);
            spi_bus_free(config->spi_host);
            vQueueDelete(rx_timestamp_queue);
            return RF_TRANSPORT_STATUS_SPI_ERROR;
        }
    }

    const rf_transport_t initialized_transport = {
        .spi_device = spi_device,
        .spi_host = config->spi_host,
        .cs_gpio = config->cs_gpio,
        .miso_gpio = config->miso_gpio,
        .gdo0_gpio = config->gdo0_gpio,
        .rx_timestamp_queue = rx_timestamp_queue,
        .initialized = true,
        .gdo0_interrupt_enabled = false,
    };
    *transport = initialized_transport;

    if (config->enable_gdo0_interrupt) {
        status = gpio_install_isr_service(0);
        if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) {
            spi_bus_remove_device(spi_device);
            spi_bus_free(config->spi_host);
            vQueueDelete(rx_timestamp_queue);
            memset(transport, 0, sizeof(*transport));
            return RF_TRANSPORT_STATUS_SPI_ERROR;
        }

        status = gpio_isr_handler_add(
            config->gdo0_gpio,
            cc1101_gdo0_isr,
            transport
        );
        if (status != ESP_OK) {
            spi_bus_remove_device(spi_device);
            spi_bus_free(config->spi_host);
            vQueueDelete(rx_timestamp_queue);
            memset(transport, 0, sizeof(*transport));
            return RF_TRANSPORT_STATUS_SPI_ERROR;
        }
        transport->gdo0_interrupt_enabled = true;
    }

    return RF_TRANSPORT_STATUS_OK;
}

rf_transport_status_t rf_transport_read_chip_info(
    const rf_transport_t *transport,
    rf_transport_chip_info_t *out_info
)
{
    if (transport == NULL || out_info == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }

    rf_transport_chip_info_t info = {0};

    rf_transport_status_t status = read_status_register(
        transport,
        CC1101_PARTNUM_ADDR,
        &info.partnum
    );
    if (status != RF_TRANSPORT_STATUS_OK) {
        return status;
    }

    status = read_status_register(
        transport,
        CC1101_VERSION_ADDR,
        &info.version
    );
    if (status != RF_TRANSPORT_STATUS_OK) {
        return status;
    }

    *out_info = info;
    return RF_TRANSPORT_STATUS_OK;
}

rf_transport_status_t rf_transport_configure_433mhz(
    const rf_transport_t *transport
)
{
    if (transport == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (!transport->initialized) {
        return RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }

    rf_transport_status_t status = reset_radio(transport);
    if (status != RF_TRANSPORT_STATUS_OK) {
        return status;
    }

    for (size_t i = 0U; i < sizeof(s_433mhz_settings) / sizeof(s_433mhz_settings[0]); ++i) {
        status = write_register(
            transport,
            s_433mhz_settings[i].address,
            s_433mhz_settings[i].value
        );
        if (status != RF_TRANSPORT_STATUS_OK) {
            return status;
        }
    }

    /* VERSION 값만으로 정품/호환 CC1101을 판정하지 않는다. IOCFG2는 방금
     * 0x29를 기록한 설정 레지스터이므로 이를 다시 읽어야 MOSI와 MISO 양쪽이
     * 실제로 동작했음을 확인할 수 있다. MISO가 LOW에 고정되면 0x00이 읽혀
     * 여기에서 명확하게 실패한다. */
    uint8_t iocfg2_readback = 0U;
    status = read_config_register(transport, 0x00U, &iocfg2_readback);
    if (status != RF_TRANSPORT_STATUS_OK || iocfg2_readback != 0x29U) {
        ESP_LOGE(TAG,
                 "CC1101 register read-back failed: IOCFG2 expected=0x29 actual=0x%02X status=%s(%d)",
                 iocfg2_readback, status_name(status), status);
        return status == RF_TRANSPORT_STATUS_OK
            ? RF_TRANSPORT_STATUS_SPI_ERROR
            : status;
    }
    ESP_LOGI(TAG, "CC1101 register read-back OK: IOCFG2=0x%02X",
             iocfg2_readback);

    /* 재배정(2026-08-17): 근접 테스트용 최소출력(-30dBm, 0x12)으로는 50m급
     * 거리에서 안정적으로 안 잡혀서, 433MHz PATABLE 표 기준 이 칩의 최대
     * 출력 단계인 10dBm(0xC0)으로 올림 — 실외 시야 확보 조건에서 자유공간
     * 손실만 보면 -30dBm으로도 50m가 이론상 되지만, 실제로는 안테나 효율/
     * 장애물/PCB 배치 손실이 커서 최대 출력이 필요했음. 전파법상 출력 상한을
     * 아직 확인 안 했으니 실외 필드테스트 이후 규제값에 맞춰 다시 낮춰야 할
     * 수 있음(TODO). */
    const uint8_t pa_table = 0xC0U;
    status = write_register(transport, CC1101_PATABLE_ADDR, pa_table);
    if (status != RF_TRANSPORT_STATUS_OK) {
        return status;
    }

    status = command_strobe(transport, CC1101_SIDLE);
    if (status == RF_TRANSPORT_STATUS_OK) {
        status = command_strobe(transport, CC1101_SFRX);
    }
    if (status == RF_TRANSPORT_STATUS_OK) {
        status = command_strobe(transport, CC1101_SFTX);
    }
    return status;
}

rf_transport_status_t rf_transport_recover_433mhz(
    const rf_transport_t *transport
)
{
    if (transport == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (!transport->initialized) {
        return RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }

    /* 역할 전환은 다른 core에서 SPI 중인 service task를 종료할 수 있다. 그
     * 순간 CS가 LOW였다면 다음 strobe가 새 command로 인식되지 않으므로 먼저
     * CS를 확실히 해제하고 한 tick 뒤 CC1101 설정을 처음부터 다시 적용한다. */
    gpio_set_level(transport->cs_gpio, 1);
    vTaskDelay(1U);
    if (transport->rx_timestamp_queue != NULL) {
        xQueueReset((QueueHandle_t)transport->rx_timestamp_queue);
    }
    return rf_transport_configure_433mhz(transport);
}

rf_transport_status_t rf_transport_set_channel(
    const rf_transport_t *transport,
    uint8_t channel
)
{
    if (transport == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (!transport->initialized) {
        return RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }

    rf_transport_status_t status = command_strobe(transport, CC1101_SIDLE);
    if (status == RF_TRANSPORT_STATUS_OK) {
        status = write_register(transport, CC1101_CHANNR_ADDR, channel);
    }
    if (status == RF_TRANSPORT_STATUS_OK) {
        status = command_strobe(transport, CC1101_SFRX);
    }
    if (status == RF_TRANSPORT_STATUS_OK) {
        status = command_strobe(transport, CC1101_SFTX);
    }
    if (status == RF_TRANSPORT_STATUS_OK &&
        transport->rx_timestamp_queue != NULL) {
        /* A slot may contain SYNC followed by audio packets. Discard the
         * previous channel's last GDO0 edge together with its flushed RX FIFO;
         * otherwise that stale timestamp can be paired with the next slot's
         * SYNC packet and appear almost one slot early. */
        xQueueReset((QueueHandle_t)transport->rx_timestamp_queue);
    }
    return status;
}

rf_transport_status_t rf_transport_start_receive(
    const rf_transport_t *transport
)
{
    if (transport == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (!transport->initialized) {
        return RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }
    return command_strobe(transport, CC1101_SRX);
}

rf_transport_status_t rf_transport_send_packet(
    const rf_transport_t *transport,
    const uint8_t *payload,
    uint8_t length
)
{
    if (transport == NULL || payload == NULL || length == 0U ||
        length > RF_TRANSPORT_MAX_PACKET_LENGTH) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (!transport->initialized) {
        return RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }

    rf_transport_status_t status = command_strobe(transport, CC1101_SIDLE);
    if (status == RF_TRANSPORT_STATUS_OK) {
        status = command_strobe(transport, CC1101_SFTX);
    }
    if (status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "TX prepare failed: status=%s(%d)",
                 status_name(status), status);
        return status;
    }

    uint8_t fifo_data[RF_TRANSPORT_MAX_PACKET_LENGTH + 1U] = {0};
    fifo_data[0] = length;
    memcpy(&fifo_data[1], payload, length);

    status = write_burst(transport, CC1101_FIFO_ADDR, fifo_data, length + 1U);
    if (status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "TX FIFO write failed: status=%s(%d) length=%u",
                 status_name(status), status, length);
        return status;
    }

    status = command_strobe(transport, CC1101_STX);
    if (status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "STX strobe failed: status=%s(%d)",
                 status_name(status), status);
        return status;
    }

    const int64_t deadline_us = esp_timer_get_time() + CC1101_PACKET_TIMEOUT_US;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t marc_state = 0U;
        status = read_status_register(transport, CC1101_MARCSTATE_ADDR, &marc_state);
        if (status != RF_TRANSPORT_STATUS_OK) {
            return status;
        }
        if ((marc_state & CC1101_MARCSTATE_MASK) == CC1101_MARCSTATE_IDLE) {
            uint8_t tx_bytes = 0U;
            status = read_status_register(transport, CC1101_TXBYTES_ADDR, &tx_bytes);
            if (status != RF_TRANSPORT_STATUS_OK) {
                ESP_LOGE(TAG, "TXBYTES read failed: status=%s(%d)",
                         status_name(status), status);
                return status;
            }
            if ((tx_bytes & CC1101_FIFO_ERROR_MASK) != 0U) {
                ESP_LOGE(TAG, "TX FIFO underflow: MARCSTATE=0x%02X TXBYTES=0x%02X",
                         marc_state, tx_bytes);
                return RF_TRANSPORT_STATUS_SPI_ERROR;
            }
            /* STX can be accepted at the SPI boundary while the radio stays
             * IDLE (for example when calibration/TX never starts).  The old
             * code treated that first stale-IDLE sample as successful even
             * though the complete frame was still queued.  A transmission is
             * complete only after the FIFO has drained to zero. */
            if ((tx_bytes & CC1101_FIFO_COUNT_MASK) == 0U) {
                return RF_TRANSPORT_STATUS_OK;
            }
        }
        vTaskDelay(1U);
    }

    uint8_t final_marc_state = 0xFFU;
    uint8_t final_tx_bytes = 0xFFU;
    const rf_transport_status_t marc_status = read_status_register(
        transport, CC1101_MARCSTATE_ADDR, &final_marc_state);
    const rf_transport_status_t bytes_status = read_status_register(
        transport, CC1101_TXBYTES_ADDR, &final_tx_bytes);
    ESP_LOGE(TAG,
             "TX timeout: length=%u MARCSTATE=0x%02X(%s) TXBYTES=0x%02X(%s)",
             length,
             final_marc_state, status_name(marc_status),
             final_tx_bytes, status_name(bytes_status));
    (void)command_strobe(transport, CC1101_SIDLE);
    (void)command_strobe(transport, CC1101_SFTX);
    return RF_TRANSPORT_STATUS_TIMEOUT;
}

rf_transport_status_t rf_transport_receive_packet(
    const rf_transport_t *transport,
    uint32_t timeout_ms,
    rf_transport_rx_packet_t *out_packet
)
{
    if (transport == NULL || out_packet == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (!transport->initialized) {
        return RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }

    rf_transport_status_t status = command_strobe(transport, CC1101_SRX);
    if (status != RF_TRANSPORT_STATUS_OK) {
        return status;
    }

    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;

    uint8_t packet_length = 0U;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t rx_bytes = 0U;
        status = read_status_register(transport, CC1101_RXBYTES_ADDR, &rx_bytes);
        if (status != RF_TRANSPORT_STATUS_OK) {
            return status;
        }
        if ((rx_bytes & CC1101_FIFO_ERROR_MASK) != 0U) {
            command_strobe(transport, CC1101_SIDLE);
            command_strobe(transport, CC1101_SFRX);
            command_strobe(transport, CC1101_SRX);
            return RF_TRANSPORT_STATUS_SPI_ERROR;
        }
        if ((rx_bytes & 0x7FU) > 0U) {
            status = read_burst(transport, CC1101_FIFO_ADDR, &packet_length, 1U);
            if (status != RF_TRANSPORT_STATUS_OK) {
                return status;
            }
            break;
        }
        vTaskDelay(1U);
    }

    if (packet_length == 0U || packet_length > RF_TRANSPORT_MAX_PACKET_LENGTH) {
        command_strobe(transport, CC1101_SIDLE);
        command_strobe(transport, CC1101_SFRX);
        return RF_TRANSPORT_STATUS_TIMEOUT;
    }

    while (esp_timer_get_time() < deadline_us) {
        uint8_t rx_bytes = 0U;
        status = read_status_register(transport, CC1101_RXBYTES_ADDR, &rx_bytes);
        if (status != RF_TRANSPORT_STATUS_OK) {
            return status;
        }
        if ((rx_bytes & 0x7FU) >= (uint8_t)(packet_length + 2U)) {
            uint8_t packet_data[RF_TRANSPORT_MAX_PACKET_LENGTH + 2U] = {0};
            status = read_burst(
                transport,
                CC1101_FIFO_ADDR,
                packet_data,
                packet_length + 2U
            );
            if (status != RF_TRANSPORT_STATUS_OK) {
                return status;
            }

            rf_transport_rx_packet_t packet = {
                .length = packet_length,
                .rssi_dbm = (int16_t)((int8_t)packet_data[packet_length]) / 2 - 74,
                .lqi = packet_data[packet_length + 1U] & 0x7FU,
                .crc_ok = (packet_data[packet_length + 1U] & 0x80U) != 0U,
            };
            memcpy(packet.payload, packet_data, packet_length);
            *out_packet = packet;

            command_strobe(transport, CC1101_SRX);
            return RF_TRANSPORT_STATUS_OK;
        }
        vTaskDelay(1U);
    }

    command_strobe(transport, CC1101_SIDLE);
    command_strobe(transport, CC1101_SFRX);
    return RF_TRANSPORT_STATUS_TIMEOUT;
}

rf_transport_status_t rf_transport_wait_rx_timestamp(
    const rf_transport_t *transport,
    uint32_t timeout_ms,
    int64_t *out_timestamp_us
)
{
    if (transport == NULL || out_timestamp_us == NULL) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    if (!transport->initialized || !transport->gdo0_interrupt_enabled ||
        transport->rx_timestamp_queue == NULL) {
        return RF_TRANSPORT_STATUS_NOT_INITIALIZED;
    }

    int64_t timestamp_us = 0;
    const BaseType_t received = xQueueReceive(
        (QueueHandle_t)transport->rx_timestamp_queue,
        &timestamp_us,
        pdMS_TO_TICKS(timeout_ms)
    );
    if (received != pdTRUE) {
        return RF_TRANSPORT_STATUS_TIMEOUT;
    }

    *out_timestamp_us = timestamp_us;
    return RF_TRANSPORT_STATUS_OK;
}
