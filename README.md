# firmware-esp32

ESP32-S3 기반 무전기 단말 펌웨어. FHSS 음성 통신 + RF OTA 수신 담당.

## 핵심 기능
- I2S 마이크/스피커 입출력, PTT 제어
- Speex Narrowband(8kHz) 음성 압축/해제
- OLED 상태 표시 (채널/배터리/TX-RX/OTA 진행률)
- CC1101 단일 라디오로 음성 FHSS 호핑 동기화 + OTA 청크 수신·파티션 전환/롤백 겸용 (nRF24L01 이원화는 보류)

## 컴포넌트 구조 (목표)
```
components/{audio_io, audio_codec, display_ui, ota_client, fhss_core, rf_transport}
```

## 현재 브랜치 구현 현황 (`impl/speex-on-esp`)
- [x] Speex 코덱 연동
  - `components/speex` — xiph/speex 원본 (git submodule, pristine 유지)
  - `components/speex_port` — ESP-IDF 빌드용 래퍼 CMakeLists (협대역 전용 소스만 선별)
  - `main/audio_codec.h` / `main/audio_codec.c` — encode/decode 얇은 래퍼, `main.c`에서 인코딩→디코딩 스모크 테스트로 연결 확인
- [ ] I2S 마이크/스피커, PTT, OLED, FHSS 호핑, CC1101 OTA 등 나머지 기능은 아직 미구현 (스켈레톤 상태)

> 위 컴포넌트 구조는 목표(planned) 형태이며, 아직 `audio_codec`을 제외한 나머지는 `components/` 아래로 분리되지 않았다.

## 담당
팀원1 (오디오/OLED), 팀원2 (OTA/부트로더), 팀원5 (FHSS)
