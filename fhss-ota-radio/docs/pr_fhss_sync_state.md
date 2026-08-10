# PR: FHSS Sync State 구현 복구

## PR 제목

`fix: implement FHSS synchronization state tracking`

## 작업 목적

- 누락되어 있던 동기 상태 추적 공개 API와 구현을 복구한다.
- 연속 정상 수신 횟수로 동기 획득을, 연속 미수신 횟수로 동기 상실을 판정한다.
- 상태 전이가 발생한 순간에만 상위 계층이 소비할 이벤트를 반환한다.

## 주요 변경 사항

- `SEARCHING`, `LOCKED` 상태 정의
- `NONE`, `ACQUIRED`, `LOST` 이벤트 정의
- 획득 및 상실 임계값 설정 구조체 추가
- 정상 수신과 미수신 카운터 갱신 로직 구현
- 초기화 전 접근과 잘못된 설정값 검증

## 상태 전이

```text
SEARCHING -- 정상 수신 acquire_count회 --> LOCKED
LOCKED    -- 미수신 loss_count회 -------> SEARCHING
```

- 정상 수신 중간에 MISS가 발생하면 획득 카운터를 초기화한다.
- LOCKED 상태에서 정상 수신하면 상실 카운터를 초기화한다.
- 상태 전이가 없는 호출은 `FHSS_SYNC_EVENT_NONE`을 반환한다.

## 변경 파일

| 파일 | 변경 내용 |
|---|---|
| `components/fhss_core/fhss_sync_state.c` | 초기화, 정상 수신, 미수신, 상태 조회 구현 |
| `components/fhss_core/include/fhss_sync_state.h` | 상태·이벤트·설정·트래커 및 공개 API 정의 |

## 검증

- 최종 스택 브랜치에서 `ninja -C build` 성공
- ESP32-S3 애플리케이션 바이너리 생성 성공
- `git diff --check` 통과

## 리뷰 포인트

- [ ] `acquire_count`와 `loss_count`가 0일 때 설정 오류로 처리하는 것이 적절한가?
- [ ] SEARCHING 중 MISS와 LOCKED 중 VALID의 반대쪽 카운터 초기화가 의도와 일치하는가?
- [ ] 상태 전이 후 해당 카운터를 0으로 되돌리는 정책이 적절한가?
- [ ] 이벤트가 전이 순간 한 번만 발생하는가?

## 영향 범위 및 의존성

- 대상 브랜치: `fix/fhss-hop-sequence-implementation`
- 작업 브랜치: `fix/fhss-sync-state-implementation`
- 커밋: `5fb535f`
- PR diff를 Sync State 기능으로 한정하기 위한 스택 구조이며 Hop Sequence 로직에 직접 의존하지 않는다.

## 제외 범위

- FreeRTOS 이벤트 큐 전송
- OTA 수신 중 카운팅 중지·재개 정책
- 수신 타이밍 윈도우 판정
- 실제 패킷 타임아웃 스케줄링

