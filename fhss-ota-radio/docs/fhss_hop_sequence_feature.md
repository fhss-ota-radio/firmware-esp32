# FHSS Hop Sequence 기능 구현

> 슬롯 번호와 채널 배열을 입력으로 받아 결정론적인 홉 인덱스와 무선 채널을 계산하는 기능이다.

## 1. 기능 개요

- 목적: 송신기와 수신기가 같은 슬롯 번호에서 같은 채널을 선택하도록 한다.
- 담당 모듈: `components/fhss_core/fhss_hop_sequence.*`
- 시간 복잡도: 초기화 및 조회 모두 O(1)
- 동적 메모리: 사용하지 않음

## 2. 데이터 흐름

```text
채널 배열 + 채널 수
        ↓ init
fhss_hop_sequence_t
        ↓ slot_number
홉 인덱스 = slot_number % channel_count
        ↓
channels[홉 인덱스]
        ↓
현재 RF 채널
```

## 3. 공개 인터페이스

```c
fhss_hop_status_t fhss_hop_sequence_init(
    fhss_hop_sequence_t *sequence,
    const uint8_t *channels,
    size_t channel_count
);

fhss_hop_status_t fhss_hop_sequence_get_index(
    const fhss_hop_sequence_t *sequence,
    uint32_t slot_number,
    uint8_t *out_index
);

fhss_hop_status_t fhss_hop_sequence_get_channel(
    const fhss_hop_sequence_t *sequence,
    uint32_t slot_number,
    uint8_t *out_channel
);
```

## 4. 상태 코드

| 상태 | 의미 |
|---|---|
| `FHSS_HOP_STATUS_OK` | 정상 처리 |
| `FHSS_HOP_STATUS_INVALID_ARG` | 필수 포인터가 `NULL` |
| `FHSS_HOP_STATUS_INVALID_CONFIG` | 채널 수가 0이거나 256개 초과 |
| `FHSS_HOP_STATUS_NOT_INITIALIZED` | 초기화 전 조회 |

## 5. 설계 결정

- 채널 배열은 복사하지 않고 포인터로 보관한다. 호출자는 컨텍스트 사용 기간 동안 배열을 유지해야 한다.
- 슬롯 번호에 modulo를 적용하므로 동일한 입력은 항상 동일한 결과를 낸다.
- 출력 인덱스가 `uint8_t`이므로 채널 수는 최대 256개로 제한한다.
- heap과 락을 사용하지 않아 수신 처리 경로에서 호출하기 쉽다.

## 6. 변경 파일

| 파일 | 역할 |
|---|---|
| `components/fhss_core/fhss_hop_sequence.c` | Hop Sequence 계산 로직 |
| `components/fhss_core/include/fhss_hop_sequence.h` | 공개 타입과 API |

## 7. 검증 결과

| 항목 | 결과 |
|---|---|
| 전체 ESP-IDF 빌드 | 성공 |
| 애플리케이션 바이너리 생성 | 성공 |
| 실제 CC1101 채널 전환 | 미검증 |
| 단위 테스트 | 후속 작업 |

## 8. 후속 작업

1. 슬롯 경계값과 `UINT32_MAX` 입력 단위 테스트 추가
2. 중복 채널 허용 여부 및 채널 유효 범위 정책 확정
3. 시드 기반 셔플이 필요하면 별도 시퀀스 생성 계층으로 추가
4. `rf_transport`에서 계산된 채널을 CC1101 설정에 반영

## 9. Git 정보

- 브랜치: `fix/fhss-hop-sequence-implementation`
- 기준 브랜치: `develop`
- 커밋: `9d7f767`
- 후속 스택: `fix/fhss-sync-state-implementation`

