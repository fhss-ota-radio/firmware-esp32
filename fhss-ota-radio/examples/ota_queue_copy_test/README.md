# OTA queue copy test

RF 하드웨어 없이 `ota_client_submit_packet()`의 값 복사와 5청크 고정 배치의
개별 ACK 추적·선택 재전송·순차 기록을 검증하는 ESP32용 테스트 앱이다. OLED,
로터리 엔코더, 오디오와 `main/fsm.c`는 빌드하거나 초기화하지 않는다.

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
I OTA_QUEUE_TEST: ota-protocol v0.2 DATA payload 48-byte limit test PASS
I OTA_QUEUE_TEST: START image_size/total_chunks validation test PASS
I OTA_QUEUE_TEST: versionless DISCOVER/DISCOVER_ACK wire format test PASS
I OTA_QUEUE_TEST: 5-chunk individual-ACK/retransmission test PASS
I OTA_QUEUE_TEST: single-call batch write test PASS
I OTA_QUEUE_TEST: partial final batch test PASS
I OTA_QUEUE_TEST: ALL TESTS PASS
```

배치 테스트 중에는 `RX DATA`, seq별 `TX ACK`, Gateway의 ACK tracker,
`RETRY RX DATA`, 자동 배치 완료와 `ORDERED WRITE CALLBACK` 순서도 함께 출력된다.
배치 cache 단위 테스트의 콜백은 RAM의 5개 청크가 최대 240바이트의 연속
buffer로 합쳐져 writer가 배치당 한 번만 호출되는지 확인하는 테스트 대역이다.
별도의 consumer session 테스트는 실행 중인 정상 앱의 앞 240바이트를 읽어
비활성 OTA 파티션에 실제로 기록한다. END에서는 의도적으로 잘린 이미지의 검증
실패와 NACK을 확인하므로 부팅 파티션은 바뀌지 않는다.

## Raw log와 CSV 저장

테스트 펌웨어를 COM4에 플래시한 뒤 다음 명령을 실행하면 ESP32를 리셋하고
8초간 UART 로그를 수집한다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\collect_test_results.ps1 -Port COM4
```

결과는 `test-results/consumer-test-YYYYMMDD-HHMMSS.log`와 같은 이름의
`.csv`로 함께 저장된다. CSV 열은 다음과 같다.

```text
timestamp_ms,suite,test_case,status,detail
```

수집된 `TEST_CSV` 행이 없으면 exit code 2, 하나라도 FAIL이면 exit code 1,
모두 통과하면 exit code 0을 반환하므로 CI나 반복 실보드 테스트에도 사용할 수
있다.
