#pragma once

/*
 * 펌웨어 버전(major.minor.patch, 1바이트씩) — 릴리스마다 이 세 값만 올릴 것.
 * OTA_DISCOVER_ACK(components/ota_client/include/ota_discover_packet.h의
 * ota_discover_ack_t.firmware_version)에 실어 보내 Qt 앱이 스캔 응답에서
 * 기기의 현재 버전을 파악하는 데 씀. OTA_DISCOVER 패킷 자체의 version
 * 필드(패킷 규격 버전)와는 다른 값이니 혼동하지 말 것.
 */
#define FIRMWARE_VERSION_MAJOR 0
#define FIRMWARE_VERSION_MINOR 1
#define FIRMWARE_VERSION_PATCH 0
