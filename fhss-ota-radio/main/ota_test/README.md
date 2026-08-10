# RF 없는 A/B OTA 실보드 검증

이 테스트는 RF 전송 대신 `storage` 파티션에 미리 기록한 후보 앱 이미지를
`ota_client`의 세션 API로 스트리밍한다.

검증 순서는 다음과 같다.

1. `factory`가 자기 이미지를 `ota_0`에 기록하고 재부팅한다.
2. `ota_0`가 `storage`의 후보 이미지를 `ota_1`에 기록하고 재부팅한다.
3. 후보 이미지가 `ota_1`에서 실행되면 `TEST PASS`를 출력한다.

ESP-IDF 터미널에서 다음 순서로 실행한다.

```powershell
$otaPython = 'D:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe'
$idfScript = 'D:\esp\v6.0.2\esp-idf\tools\idf.py'

& $otaPython $idfScript -D 'OTA_TEST_BUILD_ID=candidate' reconfigure build
Copy-Item build\fhss-ota-radio.bin build\ota-candidate.bin -Force

& $otaPython $idfScript -D 'OTA_TEST_BUILD_ID=base' reconfigure build

Get-FileHash build\ota-candidate.bin, build\fhss-ota-radio.bin

& $otaPython -m esptool --chip esp32 -p COM4 --after no-reset `
    write-flash 0x710000 build\ota-candidate.bin

& $otaPython $idfScript -p COM4 flash monitor
```

두 이미지의 SHA-256은 서로 달라야 한다. 같다면 candidate 빌드가 생성되지
않은 것이므로 보드에 기록하지 않는다.

마지막 로그에 다음 값이 모두 있어야 성공이다.

```text
Running partition: ota_1
Stage 3/3: candidate booted from ota_1
TEST PASS: A/B OTA completed, ..., build_id=candidate
```

`idf.py flash`가 bootloader, 파티션 테이블, 초기 `otadata`, base factory 앱을
기록하지만 `storage`는 덮어쓰지 않으므로 미리 넣은 후보 이미지는 유지된다.
`--after no-reset`은 base 앱을 플래시하기 전에 현재 펌웨어가 후보 이미지를
먼저 처리하는 것을 막는다.
