#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_batch_cache.h"
#include "ota_client.h"
#include "ota_client_internal.h"
#include "ota_protocol.h"

static const char *TAG = "OTA_QUEUE_TEST";

typedef struct {
    uint8_t packet[OTA_CLIENT_MAX_PACKET_LENGTH];
    size_t length;
    volatile uint32_t count;
} send_capture_t;

static send_capture_t s_send_capture;
static volatile bool s_ota_mode;

typedef bool (*test_case_fn_t)(void);

typedef struct {
    const char *name;
    test_case_fn_t run;
    const char *detail;
} test_case_t;

static void report_test_result(
    const char *test_name,
    bool passed,
    const char *detail
)
{
    const uint32_t timestamp_ms = (uint32_t)(
        xTaskGetTickCount() * portTICK_PERIOD_MS
    );
    /* PC collector가 TEST_CSV부터 잘라서 CSV로 저장한다. detail에는 쉼표를
     * 넣지 않아 별도 quoting 없이도 PowerShell ConvertFrom-Csv가 읽는다. */
    ESP_LOGI(
        TAG,
        "TEST_CSV,%" PRIu32 ",ota_consumer,%s,%s,%s",
        timestamp_ms,
        test_name,
        passed ? "PASS" : "FAIL",
        detail
    );
}

static esp_err_t capture_send(
    const uint8_t *packet,
    size_t packet_length,
    void *context
)
{
    (void)context;
    if (packet == NULL || packet_length > sizeof(s_send_capture.packet)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_send_capture.packet, packet, packet_length);
    s_send_capture.length = packet_length;
    s_send_capture.count++;
    return ESP_OK;
}

static bool test_ota_mode(void *context)
{
    (void)context;
    return s_ota_mode;
}

static bool wait_for_send_count(uint32_t expected_count)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500U);
    while (s_send_capture.count < expected_count &&
           xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    return s_send_capture.count >= expected_count;
}

static bool submit_and_expect_ack(
    const uint8_t *packet,
    size_t packet_length,
    ota_packet_type_t expected_response_type,
    uint32_t expected_session_id,
    ota_packet_type_t expected_acknowledged_type,
    uint32_t expected_sequence,
    ota_result_t expected_result
)
{
    const uint32_t expected_count = s_send_capture.count + 1U;
    if (ota_client_submit_packet(packet, packet_length) != ESP_OK ||
        !wait_for_send_count(expected_count)) {
        ESP_LOGE(TAG, "consumer response timeout");
        return false;
    }

    ota_packet_type_t response_type;
    ota_ack_fields_t fields = {0};
    if (!ota_protocol_decode_ack(
            s_send_capture.packet,
            s_send_capture.length,
            &response_type,
            &fields
        ) ||
        response_type != expected_response_type ||
        fields.session_id != expected_session_id ||
        fields.acknowledged_type != expected_acknowledged_type ||
        fields.sequence != expected_sequence ||
        fields.result_code != expected_result) {
        ESP_LOGE(
            TAG,
            "response mismatch: type=%u ack_type=%u seq=%u result=%u",
            (unsigned)response_type,
            fields.acknowledged_type,
            (unsigned)fields.sequence,
            fields.result_code
        );
        return false;
    }
    return true;
}

static bool run_copy_test(void)
{
    uint8_t source[OTA_CLIENT_MAX_PACKET_LENGTH];
    uint8_t expected[OTA_CLIENT_MAX_PACKET_LENGTH];

    for (size_t i = 0U; i < sizeof(source); ++i) {
        source[i] = (uint8_t)i;
        expected[i] = (uint8_t)i;
    }

    if (ota_client_submit_packet(source, sizeof(source)) != ESP_OK) {
        ESP_LOGE(TAG, "submit failed");
        return false;
    }

    /* RF 드라이버가 동일한 수신 버퍼를 바로 재사용하는 상황을 재현 */
    memset(source, 0xA5, sizeof(source));

    ota_client_rx_packet_t copied = {0};
    if (ota_client_receive_packet(&copied, 0U) != ESP_OK) {
        ESP_LOGE(TAG, "dequeue failed");
        return false;
    }
    if (copied.length != sizeof(expected)) {
        ESP_LOGE(TAG, "length mismatch: expected=%u actual=%u",
                 (unsigned)sizeof(expected), copied.length);
        return false;
    }
    if (memcmp(copied.data, expected, sizeof(expected)) != 0) {
        ESP_LOGE(TAG, "copied payload changed after source buffer reuse");
        return false;
    }

    ESP_LOGI(TAG, "60-byte source buffer reuse test PASS");
    return true;
}

static bool run_length_validation_test(void)
{
    uint8_t packet[OTA_CLIENT_MAX_PACKET_LENGTH + 1U] = {0};

    if (ota_client_submit_packet(NULL, 1U) != ESP_ERR_INVALID_ARG ||
        ota_client_submit_packet(packet, 0U) != ESP_ERR_INVALID_ARG ||
        ota_client_submit_packet(packet, sizeof(packet)) != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "invalid packet length was accepted");
        return false;
    }

    ESP_LOGI(TAG, "packet length validation test PASS");
    return true;
}

static bool run_queue_capacity_test(void)
{
    uint8_t packet[1] = {0x5A};

    for (size_t i = 0U; i < OTA_CLIENT_RX_QUEUE_DEPTH; ++i) {
        if (ota_client_submit_packet(packet, sizeof(packet)) != ESP_OK) {
            ESP_LOGE(TAG, "queue filled early at item %u", (unsigned)i);
            return false;
        }
    }
    if (ota_client_submit_packet(packet, sizeof(packet)) != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "queue overflow was not reported");
        return false;
    }

    ota_client_rx_packet_t discarded = {0};
    for (size_t i = 0U; i < OTA_CLIENT_RX_QUEUE_DEPTH; ++i) {
        if (ota_client_receive_packet(&discarded, 0U) != ESP_OK) {
            ESP_LOGE(TAG, "queue drain failed at item %u", (unsigned)i);
            return false;
        }
    }

    ESP_LOGI(TAG, "queue capacity test PASS");
    return true;
}

typedef struct {
    uint8_t data[OTA_CLIENT_BATCH_SIZE * OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE];
    size_t length;
} batch_write_capture_t;

static esp_err_t capture_batch_write(
    const uint8_t *data,
    size_t data_size,
    void *context
)
{
    batch_write_capture_t *capture = (batch_write_capture_t *)context;
    if (capture == NULL || data == NULL ||
        data_size > sizeof(capture->data) - capture->length) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(&capture->data[capture->length], data, data_size);
    ESP_LOGI(TAG, "ORDERED WRITE CALLBACK: seq=%u, length=%u",
             data[0], (unsigned)data_size);
    capture->length += data_size;
    return ESP_OK;
}

static bool store_test_chunk(
    ota_batch_cache_t *cache,
    uint32_t sequence,
    uint8_t value
)
{
    const uint8_t chunk[2] = {value, (uint8_t)(value + 0x40U)};
    return ota_batch_cache_store(
        cache,
        sequence,
        chunk,
        sizeof(chunk),
        NULL
    ) == ESP_OK;
}

static bool run_batch_retransmission_test(void)
{
    ota_batch_cache_t cache = {0};
    ota_batch_cache_prepare(&cache, 0U, 8U);
    uint8_t acknowledged_mask = 0U;

    ESP_LOGI(TAG, "BATCH START: base=0, count=5, range=[0..4]");
    const uint8_t first_sequences[] = {0U, 2U, 4U};
    for (size_t i = 0U; i < sizeof(first_sequences); ++i) {
        const uint8_t sequence = first_sequences[i];
        ESP_LOGI(TAG, "RX DATA: seq=%u", sequence);
        if (!store_test_chunk(&cache, sequence, sequence)) {
            ESP_LOGE(TAG, "initial batch store failed at seq=%u", sequence);
            return false;
        }
        acknowledged_mask |= (uint8_t)(1U << sequence);
        ESP_LOGI(TAG, "TX ACK: seq=%u", sequence);
    }

    /* Gateway는 5개를 보낸 뒤 개별 ACK 수신 여부로 재전송 대상을 고른다. */
    const uint8_t first_missing_mask = ota_batch_cache_missing_mask(&cache);
    ESP_LOGI(TAG, "GATEWAY ACK TRACKER: acked_mask=0x%02X, no_ack=[1,3]",
             acknowledged_mask);
    if (first_missing_mask != 0x0AU || acknowledged_mask != 0x15U) {
        ESP_LOGE(TAG, "missing mask mismatch: expected=0x0A actual=0x%02X",
                 first_missing_mask);
        return false;
    }

    bool duplicate = false;
    const uint8_t duplicate_chunk[2] = {0xEEU, 0xEEU};
    if (ota_batch_cache_store(
            &cache, 2U, duplicate_chunk, sizeof(duplicate_chunk), &duplicate
        ) != ESP_OK || !duplicate) {
        ESP_LOGE(TAG, "duplicate chunk was not detected");
        return false;
    }
    ESP_LOGI(TAG, "DUPLICATE DATA: seq=2 ignored, TX ACK seq=2 again");

    ESP_LOGI(TAG, "RETRY RX DATA: seq=1");
    if (!store_test_chunk(&cache, 1U, 1U)) {
        ESP_LOGE(TAG, "selective retransmission did not complete batch");
        return false;
    }
    ESP_LOGI(TAG, "TX ACK: seq=1");
    ESP_LOGI(TAG, "RETRY RX DATA: seq=3");
    if (!store_test_chunk(&cache, 3U, 3U) ||
        !ota_batch_cache_is_complete(&cache)) {
        ESP_LOGE(TAG, "selective retransmission did not complete batch");
        return false;
    }
    ESP_LOGI(TAG, "TX ACK: seq=3");
    ESP_LOGI(TAG, "BATCH COMPLETE AUTOMATIC: writing seq 0..4 in order");

    batch_write_capture_t capture = {0};
    size_t written_size = 0U;
    if (ota_batch_cache_commit(
            &cache, capture_batch_write, &capture, &written_size
        ) != ESP_OK || written_size != 10U || capture.length != 10U) {
        ESP_LOGE(TAG, "batch commit failed");
        return false;
    }

    for (uint8_t sequence = 0U; sequence < OTA_CLIENT_BATCH_SIZE; ++sequence) {
        const size_t offset = (size_t)sequence * 2U;
        if (capture.data[offset] != sequence ||
            capture.data[offset + 1U] != (uint8_t)(sequence + 0x40U)) {
            ESP_LOGE(TAG, "flash write order mismatch at seq %u", sequence);
            return false;
        }
    }
    ESP_LOGI(TAG, "BATCH ADVANCE: next_sequence=5 (no BATCH_ACK packet)");

    /* 마지막 배치는 total_chunks=8이므로 seq 5~7 세 개만 요구한다. */
    ota_batch_cache_prepare(&cache, 5U, 8U);
    ESP_LOGI(TAG, "FINAL BATCH START: base=5, count=3, range=[5..7]");
    ESP_LOGI(TAG, "RX DATA: seq=7");
    ESP_LOGI(TAG, "RX DATA: seq=5");
    if (cache.chunk_count != 3U ||
        !store_test_chunk(&cache, 7U, 7U) ||
        !store_test_chunk(&cache, 5U, 5U) ||
        ota_batch_cache_missing_mask(&cache) != 0x02U) {
        ESP_LOGE(TAG, "partial final batch handling failed");
        return false;
    }
    ESP_LOGI(TAG, "TX ACK: seq=7");
    ESP_LOGI(TAG, "TX ACK: seq=5");
    ESP_LOGI(TAG, "GATEWAY ACK TRACKER: no_ack=[6]");
    ESP_LOGI(TAG, "RETRY RX DATA: seq=6");
    if (!store_test_chunk(&cache, 6U, 6U) ||
        !ota_batch_cache_is_complete(&cache)) {
        ESP_LOGE(TAG, "partial final batch retry failed");
        return false;
    }
    ESP_LOGI(TAG, "TX ACK: seq=6");
    ESP_LOGI(TAG, "FINAL BATCH COMPLETE: received_mask=0x%02X",
             cache.received_mask);
    ESP_LOGI(TAG, "BATCH ADVANCE: next_sequence=8 (no BATCH_ACK packet)");

    ESP_LOGI(TAG, "5-chunk individual-ACK/retransmission test PASS");
    ESP_LOGI(TAG, "ordered batch write-callback test PASS");
    ESP_LOGI(TAG, "partial final batch test PASS");
    return true;
}

static bool run_protocol_payload_limit_test(void)
{
    ota_batch_cache_t cache = {0};
    ota_batch_cache_prepare(&cache, 0U, 5U);

    uint8_t maximum_payload[OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE] = {0};
    uint8_t oversized_payload[OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE + 1U] = {0};

    if (OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE != 48U ||
        ota_batch_cache_store(
            &cache, 0U, maximum_payload, sizeof(maximum_payload), NULL
        ) != ESP_OK ||
        ota_batch_cache_store(
            &cache, 1U, oversized_payload, sizeof(oversized_payload), NULL
        ) != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "ota-protocol v0.2 payload limit mismatch");
        return false;
    }

    ESP_LOGI(TAG, "ota-protocol v0.2 DATA payload 48-byte limit test PASS");
    return true;
}

static bool run_start_metadata_validation_test(void)
{
    const uint8_t sha256[32] = {0};
    /* 49B image needs ceil(49 / 48) == 2 DATA chunks. The writer must not
     * start when the Gateway advertises an inconsistent chunk count. */
    if (ota_client_start_session(1U, 49U, 1U, sha256) != ESP_ERR_INVALID_SIZE ||
        ota_client_get_state() != OTA_CLIENT_STATE_IDLE) {
        ESP_LOGE(TAG, "inconsistent START metadata was accepted");
        return false;
    }

    ESP_LOGI(TAG, "START image_size/total_chunks validation test PASS");
    return true;
}

static bool run_discover_wire_format_test(void)
{
    uint8_t discover[OTA_DISCOVER_PACKET_SIZE] = {0};
    const uint8_t legacy_discover[] = {1U, OTA_PKT_DISCOVER};

    if (ota_protocol_encode_discover(discover, sizeof(discover)) !=
            OTA_DISCOVER_PACKET_SIZE ||
        !ota_protocol_decode_discover(discover, sizeof(discover)) ||
        ota_protocol_decode_discover(
            legacy_discover, sizeof(legacy_discover)
        )) {
        ESP_LOGE(TAG, "DISCOVER 1-byte wire format mismatch");
        return false;
    }

    const ota_discover_ack_fields_t ack = {
        .device_id = 0x00AABBCCU,
        .fw_major = 1U,
        .fw_minor = 2U,
        .fw_patch = 3U,
    };
    const uint8_t expected[] = {
        OTA_PKT_DISCOVER_ACK, 0xCCU, 0xBBU, 0xAAU, 1U, 2U, 3U
    };
    uint8_t encoded[OTA_DISCOVER_ACK_PACKET_SIZE] = {0};

    const size_t encoded_length = ota_protocol_encode_discover_ack(
        encoded, sizeof(encoded), &ack
    );
    if (
        encoded_length != sizeof(expected) ||
        memcmp(encoded, expected, sizeof(expected)) != 0) {
        ESP_LOGE(TAG, "DISCOVER_ACK 7-byte wire format mismatch");
        return false;
    }

    ESP_LOGI(TAG, "versionless DISCOVER/DISCOVER_ACK wire format test PASS");
    return true;
}

static bool run_consumer_wire_test(void)
{
    if (ota_client_start_consumer() != ESP_OK) {
        ESP_LOGE(TAG, "consumer task start failed");
        return false;
    }

    s_ota_mode = true;
    uint8_t discover[OTA_DISCOVER_PACKET_SIZE];
    const size_t discover_length = ota_protocol_encode_discover(
        discover, sizeof(discover)
    );
    if (ota_client_submit_packet(discover, discover_length) != ESP_OK ||
        !wait_for_send_count(1U)) {
        ESP_LOGE(TAG, "consumer did not answer DISCOVER");
        return false;
    }

    ota_discover_ack_fields_t discover_ack = {0};
    if (!ota_protocol_decode_discover_ack(
            s_send_capture.packet,
            s_send_capture.length,
            &discover_ack
        ) ||
        discover_ack.device_id != 0x00AABBCCU ||
        discover_ack.fw_major != 1U ||
        discover_ack.fw_minor != 2U ||
        discover_ack.fw_patch != 3U) {
        ESP_LOGE(TAG, "consumer DISCOVER_ACK fields mismatch");
        return false;
    }
    ESP_LOGI(TAG, "CONSUMER RX DISCOVER -> TX DISCOVER_ACK test PASS");

    s_ota_mode = false;
    const ota_start_fields_t start = {
        .session_id = 0x12345678U,
        .target_device_id = 0x00AABBCCU,
        .image_size = OTA_MAX_PAYLOAD_SIZE,
        .total_chunks = 1U,
        .image_sha256 = {0},
    };
    uint8_t start_packet[OTA_START_PACKET_SIZE];
    const size_t start_length = ota_protocol_encode_start(
        start_packet, sizeof(start_packet), &start
    );
    if (ota_client_submit_packet(start_packet, start_length) != ESP_OK ||
        !wait_for_send_count(2U)) {
        ESP_LOGE(TAG, "consumer did not reject START outside OTA mode");
        return false;
    }

    ota_packet_type_t response_type;
    ota_ack_fields_t nack = {0};
    if (!ota_protocol_decode_ack(
            s_send_capture.packet,
            s_send_capture.length,
            &response_type,
            &nack
        ) ||
        response_type != OTA_PKT_NACK ||
        nack.session_id != start.session_id ||
        nack.acknowledged_type != OTA_PKT_START ||
        nack.sequence != OTA_CONTROL_SEQUENCE ||
        nack.result_code != OTA_RESULT_BUSY) {
        ESP_LOGE(TAG, "consumer START BUSY NACK fields mismatch");
        return false;
    }
    ESP_LOGI(TAG, "CONSUMER RX START outside MENU_OTA -> TX BUSY NACK test PASS");
    return true;
}

static bool run_consumer_session_test(void)
{
    enum {
        TEST_CHUNK_COUNT = OTA_CLIENT_BATCH_SIZE,
        TEST_IMAGE_SIZE = TEST_CHUNK_COUNT * OTA_MAX_PAYLOAD_SIZE,
    };
    const uint32_t session_id = 0xA1B2C3D4U;
    uint8_t image[TEST_IMAGE_SIZE];
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL ||
        esp_partition_read(running, 0U, image, sizeof(image)) != ESP_OK) {
        ESP_LOGE(TAG, "failed to read running image fixture");
        return false;
    }

    s_ota_mode = true;
    const ota_start_fields_t start = {
        .session_id = session_id,
        .target_device_id = 0x00AABBCCU,
        .image_size = sizeof(image),
        .total_chunks = TEST_CHUNK_COUNT,
        .image_sha256 = {0},
    };
    uint8_t packet[OTA_CLIENT_MAX_PACKET_LENGTH];
    size_t packet_length = ota_protocol_encode_start(
        packet, sizeof(packet), &start
    );
    if (!submit_and_expect_ack(
            packet,
            packet_length,
            OTA_PKT_ACK,
            session_id,
            OTA_PKT_START,
            OTA_CONTROL_SEQUENCE,
            OTA_RESULT_OK
        ) ||
        ota_client_get_state() != OTA_CLIENT_STATE_RECEIVING) {
        ESP_LOGE(TAG, "valid START was not accepted");
        return false;
    }
    ESP_LOGI(TAG, "SESSION TEST: START ACK PASS");

    const uint8_t first_sequences[] = {0U, 2U, 4U};
    for (size_t i = 0U; i < sizeof(first_sequences); ++i) {
        const uint32_t sequence = first_sequences[i];
        packet_length = ota_protocol_encode_data(
            packet,
            sizeof(packet),
            session_id,
            sequence,
            &image[sequence * OTA_MAX_PAYLOAD_SIZE],
            OTA_MAX_PAYLOAD_SIZE
        );
        if (!submit_and_expect_ack(
                packet,
                packet_length,
                OTA_PKT_ACK,
                session_id,
                OTA_PKT_DATA,
                sequence,
                OTA_RESULT_OK
            )) {
            return false;
        }
    }
    ESP_LOGI(TAG, "SESSION TEST: out-of-order DATA [0,2,4] ACK PASS");

    packet_length = ota_protocol_encode_data(
        packet,
        sizeof(packet),
        session_id,
        1U,
        &image[OTA_MAX_PAYLOAD_SIZE],
        OTA_MAX_PAYLOAD_SIZE
    );
    packet[OTA_DATA_HEADER_SIZE] ^= 0xFFU;
    if (!submit_and_expect_ack(
            packet,
            packet_length,
            OTA_PKT_NACK,
            session_id,
            OTA_PKT_DATA,
            1U,
            OTA_RESULT_INVALID_CRC
        )) {
        return false;
    }
    ESP_LOGI(TAG, "SESSION TEST: corrupted DATA CRC NACK PASS");

    packet_length = ota_protocol_encode_data(
        packet,
        sizeof(packet),
        session_id,
        1U,
        &image[OTA_MAX_PAYLOAD_SIZE],
        OTA_MAX_PAYLOAD_SIZE
    );
    if (!submit_and_expect_ack(
            packet,
            packet_length,
            OTA_PKT_ACK,
            session_id,
            OTA_PKT_DATA,
            1U,
            OTA_RESULT_OK
        )) {
        return false;
    }

    const uint32_t timeout_response_count = s_send_capture.count + 1U;
    vTaskDelay(pdMS_TO_TICKS(1100U));
    if (s_send_capture.count < timeout_response_count) {
        ESP_LOGE(TAG, "missing sequence timeout NACK was not sent");
        return false;
    }
    ota_packet_type_t timeout_type;
    ota_ack_fields_t timeout_nack = {0};
    if (!ota_protocol_decode_ack(
            s_send_capture.packet,
            s_send_capture.length,
            &timeout_type,
            &timeout_nack
        ) ||
        timeout_type != OTA_PKT_NACK ||
        timeout_nack.session_id != session_id ||
        timeout_nack.acknowledged_type != OTA_PKT_DATA ||
        timeout_nack.sequence != 3U ||
        timeout_nack.result_code != OTA_RESULT_TIMEOUT) {
        ESP_LOGE(TAG, "missing sequence timeout NACK mismatch");
        return false;
    }
    ESP_LOGI(TAG, "SESSION TEST: missing seq=3 TIMEOUT NACK PASS");

    packet_length = ota_protocol_encode_data(
        packet,
        sizeof(packet),
        session_id,
        3U,
        &image[3U * OTA_MAX_PAYLOAD_SIZE],
        OTA_MAX_PAYLOAD_SIZE
    );
    if (!submit_and_expect_ack(
            packet,
            packet_length,
            OTA_PKT_ACK,
            session_id,
            OTA_PKT_DATA,
            3U,
            OTA_RESULT_OK
        )) {
        return false;
    }
    ESP_LOGI(TAG, "SESSION TEST: selective retry seq=3 and Flash write PASS");

    packet_length = ota_protocol_encode_data(
        packet,
        sizeof(packet),
        session_id,
        2U,
        &image[2U * OTA_MAX_PAYLOAD_SIZE],
        OTA_MAX_PAYLOAD_SIZE
    );
    if (!submit_and_expect_ack(
            packet,
            packet_length,
            OTA_PKT_ACK,
            session_id,
            OTA_PKT_DATA,
            2U,
            OTA_RESULT_OK
        )) {
        return false;
    }
    ESP_LOGI(TAG, "SESSION TEST: duplicate seq=2 ignored and ACK repeated PASS");

    const ota_end_fields_t end = {
        .session_id = session_id,
        .image_size = sizeof(image),
        .total_chunks = TEST_CHUNK_COUNT,
    };
    packet_length = ota_protocol_encode_end(packet, sizeof(packet), &end);
    if (!submit_and_expect_ack(
            packet,
            packet_length,
            OTA_PKT_NACK,
            session_id,
            OTA_PKT_END,
            OTA_CONTROL_SEQUENCE,
            OTA_RESULT_VERIFY_FAILED
        ) ||
        ota_client_get_state() != OTA_CLIENT_STATE_ERROR) {
        ESP_LOGE(TAG, "truncated image END verification test failed");
        return false;
    }
    ESP_LOGI(TAG, "SESSION TEST: invalid/truncated image END NACK PASS");

    if (ota_client_abort() != ESP_OK ||
        ota_client_get_state() != OTA_CLIENT_STATE_IDLE) {
        ESP_LOGE(TAG, "abort did not restore IDLE state");
        return false;
    }
    ESP_LOGI(TAG, "SESSION TEST: abort -> IDLE recovery PASS");
    return true;
}

void app_main(void)
{
    const ota_client_config_t config = {
        .device_id = 0x00AABBCCU,
        .firmware_version = {1U, 2U, 3U},
        .receive_timeout_ms = 1000U,
        .send_callback = capture_send,
        .ota_mode_callback = test_ota_mode,
    };

    if (ota_client_init(&config) != ESP_OK) {
        ESP_LOGE(TAG, "ota_client_init failed");
        report_test_result(
            "ota_client_init", false, "component initialization failed"
        );
        return;
    }

    const test_case_t test_cases[] = {
        {
            "queue_copy_60_bytes",
            run_copy_test,
            "RF source buffer reuse preserves queued bytes",
        },
        {
            "packet_length_validation",
            run_length_validation_test,
            "zero null and over-60-byte packets rejected",
        },
        {
            "queue_capacity",
            run_queue_capacity_test,
            "queue depth and overflow result verified",
        },
        {
            "payload_limit_48_bytes",
            run_protocol_payload_limit_test,
            "60-byte RF body minus 12-byte DATA header",
        },
        {
            "start_metadata_validation",
            run_start_metadata_validation_test,
            "image size and total chunk count consistency",
        },
        {
            "discover_wire_format",
            run_discover_wire_format_test,
            "versionless DISCOVER and 7-byte DISCOVER_ACK",
        },
        {
            "batch_selective_repeat",
            run_batch_retransmission_test,
            "five-chunk cache duplicate and final partial batch",
        },
        {
            "consumer_wire_control",
            run_consumer_wire_test,
            "DISCOVER_ACK and START BUSY NACK",
        },
        {
            "consumer_session_flow",
            run_consumer_session_test,
            "START DATA CRC timeout retry duplicate END and abort",
        },
    };

    bool passed = true;
    for (size_t i = 0U;
         i < sizeof(test_cases) / sizeof(test_cases[0]);
         ++i) {
        const bool case_passed = test_cases[i].run();
        report_test_result(
            test_cases[i].name,
            case_passed,
            test_cases[i].detail
        );
        passed = passed && case_passed;

        /* consumer task 생성 실패 뒤 session 테스트를 계속하면 Queue 소비자가
         * 없어 응답 timeout만 반복되므로 그 경우만 명시적으로 SKIP한다. */
        if (!case_passed &&
            test_cases[i].run == run_consumer_wire_test) {
            report_test_result(
                "consumer_session_flow",
                false,
                "skipped because consumer wire control failed"
            );
            break;
        }
    }

    if (passed) {
        ESP_LOGI(TAG, "ALL TESTS PASS");
    } else {
        ESP_LOGE(TAG, "TEST FAILED");
    }
}
