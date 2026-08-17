# ota_rf_bridge

`rf_transport`가 CC1101 RX FIFO에서 꺼낸 패킷을 `ota_client`의 전용 큐로
전달하는 얇은 어댑터다.

`ota_rf_bridge_receive_once()`는 다음 순서로 동작한다.

1. 기존 `rf_transport_receive_packet()`으로 패킷 하나를 받는다.
2. CC1101이 덧붙인 `crc_ok` 상태를 확인한다.
3. 길이가 `1..60` 바이트인지 확인한다.
4. `ota_client_submit_packet()`을 호출해 payload를 OTA 큐로 값 복사한다.

라디오 초기화, 채널 선택과 `rf_transport_t` 소유권은 이 컴포넌트가 갖지 않는다.
음성 FHSS와 OTA가 같은 CC1101을 공유하므로 최상위 FSM/FHSS 서비스가 사용권을
전환한 뒤 이미 초기화된 `rf_transport_t`를 넘겨야 한다.

OTA 패킷의 DISCOVER/START/DATA/END 디코딩과 ACK/NACK 처리는
`ota_client`의 `ota_consumer` 태스크가 공용 `ota_protocol.h`로 수행한다.
