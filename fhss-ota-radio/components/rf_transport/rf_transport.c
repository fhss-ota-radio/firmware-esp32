#include "rf_transport.h"

#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CC1101_PARTNUM_ADDR       0x30U
#define CC1101_VERSION_ADDR       0x31U
#define CC1101_READ_BURST         0xC0U
#define CC1101_READY_TIMEOUT_US   10000LL
#define CC1101_PACKET_TIMEOUT_US  100000LL

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

typedef struct {
    uint8_t address;
    uint8_t value;
} cc1101_register_setting_t;

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
        return RF_TRANSPORT_STATUS_SPI_ERROR;
    }

    const rf_transport_t initialized_transport = {
        .spi_device = spi_device,
        .spi_host = config->spi_host,
        .cs_gpio = config->cs_gpio,
        .miso_gpio = config->miso_gpio,
        .initialized = true,
    };
    *transport = initialized_transport;

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

    rf_transport_status_t status = command_strobe(transport, CC1101_SRES);
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

    /* Minimum output power for the first close-range test (about -30 dBm). */
    const uint8_t pa_table = 0x12U;
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
    return status;
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
        return status;
    }

    uint8_t fifo_data[RF_TRANSPORT_MAX_PACKET_LENGTH + 1U] = {0};
    fifo_data[0] = length;
    memcpy(&fifo_data[1], payload, length);

    status = write_burst(transport, CC1101_FIFO_ADDR, fifo_data, length + 1U);
    if (status != RF_TRANSPORT_STATUS_OK) {
        return status;
    }

    status = command_strobe(transport, CC1101_STX);
    if (status != RF_TRANSPORT_STATUS_OK) {
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
                return status;
            }
            return (tx_bytes & CC1101_FIFO_ERROR_MASK) == 0U
                ? RF_TRANSPORT_STATUS_OK
                : RF_TRANSPORT_STATUS_SPI_ERROR;
        }
        vTaskDelay(1U);
    }

    command_strobe(transport, CC1101_SIDLE);
    command_strobe(transport, CC1101_SFTX);
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
