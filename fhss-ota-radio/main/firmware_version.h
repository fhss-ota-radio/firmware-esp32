#pragma once

/*
 * 펌웨어 버전(major.minor.patch, 1바이트씩) — 릴리스마다 이 세 값만 올릴 것.
 * OTA_DISCOVER_ACK(components/ota_client/include/ota_discover_packet.h의
 * ota_discover_ack_t의 fw_major/minor/patch에 실어 보내 Qt 앱이 스캔
 * 응답에서 기기의 현재 버전을 파악하는 데 쓴다. 패킷 규격 version 필드는
 * ota-protocol v0.2 결정에 따라 사용하지 않는다.
 */
#define FIRMWARE_VERSION_MAJOR 0
#define FIRMWARE_VERSION_MINOR 1
#define FIRMWARE_VERSION_PATCH 0
