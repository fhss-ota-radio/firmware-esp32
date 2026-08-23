# ESP32 FHSS OTA 통신 로그 수집

이 빌드는 실제 CC1101 송수신 경로를 `OTA_DIAG` 태그로 기록한다. 반드시 새
펌웨어를 flash한 뒤 ESP32를 OTA 메뉴에 두고 Gateway 테스트를 실행한다.

## PowerShell

```powershell
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
idf.py -p COM9 flash monitor 2>&1 |
    Tee-Object -FilePath "esplog_fhss_$timestamp.txt"
```

이미 flash했다면 `flash`를 빼고 `monitor`만 실행한다. 모니터 종료는 `Ctrl+]`다.

## 핵심 로그

- `LISTEN no-packet`: OTA 리스너는 살아 있지만 CC1101에서 유효 패킷이 안 들어옴
- `RX_RESULT`: 실제 RF 수신 결과, 채널, CRC, RSSI, LQI
- `RX ... FHSS_CONFIG`: CONFIG가 ESP32 무선 계층까지 도착함
- `CONFIG ...`: CONFIG의 session/target/generation/seed/채널 정책
- `FHSS_CONFIG ignored`: target device ID 불일치
- `FHSS_CONFIG rejected`: wire format, OTA FSM 모드 또는 NVS 검증 실패
- `TX ... ACK`와 `TX_RESULT status=0`: ESP32 ACK가 실제 송신 경로에서 성공
- `RX ... FHSS_ACTIVATE`: ACTIVATE 도착
- `FHSS_ACTIVATE ACK sent`: ACK 후 rendezvous 채널로 전환 시작
- `RX ... FHSS_SYNC`: MASTER SYNC가 CC1101에서 실제 수신됨
- `SYNC_ACQUIRED`: 동일 generation/호핑 순서/타이밍 검증 완료
- `FHSS_SYNC_TIMEOUT`: 5초 동안 동기 획득 실패, 채널 0 복귀

## 빠른 판정

1. `LISTEN`만 반복되면 Gateway RF 송신, 물리 채널 또는 CC1101 RF profile 문제다.
2. `RX_RESULT crc=0`이면 주파수·sync word·modem register·신호 품질을 비교한다.
3. CONFIG는 보이지만 ACK가 없으면 이어지는 `ignored/rejected` 이유를 확인한다.
4. ACK까지 성공하고 SYNC가 전혀 안 보이면 Gateway MASTER 시작/RF profile 문제다.
5. SYNC가 보이지만 `SYNC_ACQUIRED`가 없으면 generation, hop index, slot 또는 타이밍 문제다.

로그 전달 시 `OTA_DIAG`, `ota_consumer`, `fhss_service`, `fsm` 행을 함께 보낸다.
