## PR 제목

`feat: FHSS 타이밍 윈도우 평가 기능 추가`

## 작업 목적

- FHSS 수신 타이밍 오차를 기준으로 `before`/`inside`/`after` 분류 기능을 추가
- `fhss_core`에서 동기화 타이밍 평가를 별도 모듈로 분리
- `NULL` 입력 안전성과 결과 출력을 명확히 처리

## 주요 변경사항

- `components/fhss_core/fhss_timing_window.c` 생성
- `components/fhss_core/include/fhss_timing_window.h` 생성
- `fhss_timing_window_evaluate()` 구현: 타이밍 오차 계산, 윈도우 판정, 출력 구조체 복사

## 변경 파일

| 파일 | 변경 내용 |
|---|---|
| `components/fhss_core/fhss_timing_window.c` | 타이밍 윈도우 평가 로직 구현 |
| `components/fhss_core/include/fhss_timing_window.h` | 공개 타입 및 함수 선언 추가 |

## 검증

- 빌드: 미실행
- 소프트웨어 테스트: 미실행
- 실제 하드웨어 테스트: 미실행
- 미실행 테스트:
  - 전체 프로젝트 빌드
  - 단위 경계값 테스트
  - CC1101/ESP32 실장 검증

```powershell
정보 미제공
```

## 리뷰 포인트

- [ ] `fhss_timing_window_evaluate()` 입력/출력 및 반환값 처리 적절성
- [ ] `NULL` 포인터 검사와 출력 버퍼 복사가 안전한지
- [ ] `early_margin_us`와 `late_margin_us` 필드 의미가 구현과 일치하는지
- [ ] `fhss_core` 내부 모듈 책임이 적절히 분리되어 있는지
- [ ] 기존 `fhss_core` 인터페이스와 충돌 가능성 여부

## 영향 범위

- 영향받는 모듈: `components/fhss_core`
- 인터페이스 변경: `fhss_timing_window.h` 공개 함수 추가
- 빌드 환경 변경: 없음 확인 필요
- 하드웨어 동작 변경: FHSS 수신 타이밍 판정 로직 영향
- 의존 브랜치: `feature/fhss-timing-window` (기준 브랜치 확인 필요)

## 제한사항 및 후속 작업

- 이번 PR에는 실제 테스트 로그가 포함되지 않음
- `early_margin_us`가 현재 구현에 반영되지 않음
- 다음 PR에서 단위 테스트와 하드웨어 검증 필요

## 관련 정보

- Issue: 정보 미제공
- 기준 브랜치: 확인 필요 (`develop` 예상)
- 관련 커밋: 정보 미제공
- Notion 문서: FHSS 타이밍 윈도우 평가 기능 구현 문서 작성됨
