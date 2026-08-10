# PR: FHSS Core 모듈 통합

## PR 제목

`feat: integrate FHSS core modules`

## 작업 목적

- 개별 구현된 Sync Packet, Timing Window, Hop Sequence, Sync State를 하나의 공개 API로 통합한다.
- 수신 패킷 처리 결과로 디코딩 정보, 타이밍 판정, 동기 상태, 홉 인덱스와 채널을 한 번에 제공한다.
- 패킷 미수신 타임아웃을 동기 상태의 MISS 입력으로 처리한다.

## 주요 변경 사항

- `fhss_core_t`, 설정, 처리 결과 및 상태 코드 정의
- 하위 모듈을 원자적으로 초기화하는 `fhss_core_init()` 구현
- 수신 SYNC 패킷 통합 처리 함수 구현
- 타임아웃 처리 및 현재 슬롯 채널 조회 함수 구현
- ESP-IDF 컴포넌트 CMake에 전체 FHSS 소스 등록

## 처리 흐름

```text
raw SYNC packet
    ↓ decode
수신 타이밍 평가
    ↓ inside: VALID / outside: MISS
동기 상태 갱신
    ↓
slot_number 기반 홉 인덱스·채널 계산
    ↓
fhss_core_rx_result_t 반환
```

## 변경 파일

| 파일 | 변경 내용 |
|---|---|
| `components/fhss_core/fhss_core.c` | 초기화, 수신 처리, 타임아웃, 채널 조회 구현 |
| `components/fhss_core/include/fhss_core.h` | 통합 설정·상태·결과 및 공개 API 정의 |
| `components/fhss_core/CMakeLists.txt` | FHSS Core 관련 소스 등록 |

## 오류 매핑

| 하위 모듈 | Core 상태 |
|---|---|
| Sync Packet | `FHSS_CORE_STATUS_PACKET_ERROR` |
| Timing Window | `FHSS_CORE_STATUS_TIMING_ERROR` |
| Sync State | `FHSS_CORE_STATUS_SYNC_ERROR` |
| Hop Sequence | `FHSS_CORE_STATUS_HOP_ERROR` |

## 검증

- `ninja -C build` 성공
- `fhss-ota-radio.bin` 생성 성공
- 바이너리 크기 `0x46e00`, 최소 앱 파티션 여유 72%
- `git diff --check` 통과

## 리뷰 포인트

- [ ] 초기화 실패 시 호출자의 기존 `core`를 변경하지 않는가?
- [ ] 처리 실패 시 `out_result`에 부분 결과를 남기지 않는가?
- [ ] Timing Window 밖의 유효 형식 패킷을 MISS로 처리하는 정책이 적절한가?
- [ ] 패킷의 `slot_number`를 채널 계산 기준으로 사용하는 것이 프로토콜과 일치하는가?
- [ ] 하위 모듈 오류를 Core 오류로 단순화한 수준이 충분한가?

## 영향 범위 및 의존성

- 대상 브랜치: `fix/fhss-sync-state-implementation`
- 작업 브랜치: `feature/fhss-core-integration`
- 커밋: `feec51e`
- 선행 기능: Sync Packet, Timing Window, Hop Sequence, Sync State
- `rf_transport` 및 최상위 FSM 연결은 포함하지 않는다.

## 제외 범위

- CC1101 송수신 및 채널 설정
- 슬롯 타이머와 `expected_rx_time_us` 생성
- FreeRTOS 태스크 및 이벤트 큐 연결
- OTA 모드와 FHSS 추적 간 전환
- 실제 하드웨어 통합 시험

