# OTA queue copy test

RF 하드웨어 없이 `ota_client_submit_packet()`의 값 복사와 입력 경계를 검증하는
ESP32용 테스트 앱이다. OLED, 로터리 엔코더, 오디오와 `main/fsm.c`는 빌드하거나
초기화하지 않는다.

프로젝트 본체와 동일한 8MB A/B 파티션 테이블을 사용하므로 테스트 플래시가
파티션 구성을 단일 앱 형태로 바꾸지 않는다.

```powershell
cd examples\ota_queue_copy_test
idf.py build
idf.py -p COM4 flash monitor
```

성공 시 다음 로그가 출력된다.

```text
I OTA_QUEUE_TEST: 60-byte source buffer reuse test PASS
I OTA_QUEUE_TEST: packet length validation test PASS
I OTA_QUEUE_TEST: queue capacity test PASS
I OTA_QUEUE_TEST: ALL TESTS PASS
```
