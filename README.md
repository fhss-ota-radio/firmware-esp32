# firmware-esp32

ESP32-S3 기반 무전기 단말 펌웨어. FHSS 음성 통신 + RF OTA 수신 담당.

## 핵심 기능
- I2S 마이크/스피커 입출력, PTT 제어
- Opus Narrowband(8kHz) 음성 압축/해제
- OLED 상태 표시 (채널/배터리/TX-RX/OTA 진행률)
- FHSS 호핑 시퀀스 생성 및 TX/RX 동기화
- CC1101 경유 OTA 청크 수신, 파티션 전환/롤백

## 컴포넌트 구조
```
components/{audio_io, audio_codec, display_ui, ota_client, fhss_core, rf_transport}
```

## 담당
팀원1 (오디오/OLED), 팀원2 (OTA/부트로더), 팀원5 (FHSS)
