# firmware-esp32

ESP32-S3 기반 무전기 단말 펌웨어 — FHSS 음성 통신 + RF OTA 수신 담당.

## 개요

- I2S 마이크/스피커로 음성 송수신, PTT 제어
- Speex Narrowband(8kHz) 음성 압축/해제
- OLED로 채널/배터리/TX-RX/OTA 진행률 표시
- CC1101 단일 라디오로 FHSS(주파수 도약) 음성 통신 + OTA 펌웨어 업데이트 겸용
- secret_seed 기반 HMAC-SHA256으로 세션별 홉 시드 파생(도청 시 홉 패턴 예측 방지)

## 세부 정보

이 브랜치(`main`)는 제출/배포용 요약본입니다. 빌드 방법·핀맵·필요 부품·설계 배경 등 세부 사항은 아래에서 확인하세요.

- 조직: https://github.com/fhss-ota-radio
- 개발 브랜치(최신 상태, 빌드 가이드 포함): https://github.com/fhss-ota-radio/firmware-esp32/tree/develop

## 제작자

- 설동호 — https://github.com/hiimseoll
- 조민진 — https://github.com/CHOminjin
- 김지윤 — https://github.com/JJiiyun

## 기술적 문의

설동호 (theseol16@gmail.com)
