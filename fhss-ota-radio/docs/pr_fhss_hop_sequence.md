# PR: FHSS Hop Sequence 구현 복구

## PR 제목

`fix: implement FHSS hop sequence`

## 작업 목적

- 기존에 0바이트로 남아 있던 Hop Sequence 소스와 공개 헤더를 실제 동작 가능한 상태로 복구한다.
- 슬롯 번호를 기준으로 모든 단말이 동일한 홉 인덱스와 채널을 계산하도록 한다.
- 초기화 여부와 잘못된 인자를 명확한 상태 코드로 처리한다.

## 주요 변경 사항

- `fhss_hop_sequence_t`와 `fhss_hop_status_t` 정의
- 채널 배열과 채널 수를 저장하는 초기화 API 구현
- `slot_number % channel_count` 기반의 결정론적 홉 인덱스 계산
- 홉 인덱스에 대응하는 실제 채널 조회 API 구현
- `NULL`, 빈 채널 목록, 미초기화 상태 검증

## 변경 파일

| 파일 | 변경 내용 |
|---|---|
| `components/fhss_core/fhss_hop_sequence.c` | 초기화, 인덱스 계산, 채널 조회 구현 |
| `components/fhss_core/include/fhss_hop_sequence.h` | 상태 코드, 컨텍스트, 공개 API 정의 |

## 동작 예시

채널 배열이 `{10, 20, 30}`이면 슬롯별 채널은 다음과 같다.

| 슬롯 | 홉 인덱스 | 채널 |
|---:|---:|---:|
| 0 | 0 | 10 |
| 1 | 1 | 20 |
| 2 | 2 | 30 |
| 3 | 0 | 10 |

## 검증

- 최종 스택 브랜치에서 `ninja -C build` 성공
- ESP32-S3 애플리케이션 바이너리 생성 성공
- `git diff --check` 통과

## 리뷰 포인트

- [ ] 채널 배열의 수명은 `fhss_hop_sequence_t`보다 길게 유지된다는 계약이 적절한가?
- [ ] 최대 채널 수를 256개로 제한한 것이 현재 무선 설정에 적절한가?
- [ ] 슬롯 번호의 modulo 방식이 합의된 FHSS 시퀀스 규칙과 일치하는가?
- [ ] 미초기화 및 잘못된 인자 상태 코드가 호출부에서 구분 가능한가?

## 영향 범위 및 의존성

- 대상 브랜치: `develop`
- 작업 브랜치: `fix/fhss-hop-sequence-implementation`
- 커밋: `9d7f767`
- 영향 모듈: `components/fhss_core`
- 하드웨어 설정 및 RF 송수신 동작에는 직접적인 변경 없음

## 제외 범위

- 채널 배열 생성 및 셔플 알고리즘
- 채널별 dwell time 관리
- CC1101 채널 전환
- 실제 RF 환경에서의 홉 추종 검증

