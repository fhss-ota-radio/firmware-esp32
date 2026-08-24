# FHSS 홉 시드 HMAC 파생 기능 구현

> `secret_seed`(공유 비밀)와 `public_seed`(세션마다 새로 생성, 평문 전송)를 HMAC-SHA256으로 조합해 세션별 `hop_seed`를 만드는 기능이다.

## 1. 배경 — 무엇을 고치는가

기존에는 `hop_seed`가 소스 코드 상수(`0x46485353`, ASCII "FHSS")로 고정돼 있었다. `fhss_hop_sequence`의 채널 셔플(xorshift32 기반 Fisher-Yates)은 입력 seed가 같으면 항상 같은 순서를 만들어내는 결정론적 알고리즘이므로, seed가 고정이면 **매 PTT 세션의 홉 패턴이 항상 동일하게 반복**된다. 이 기능은 세션마다 다른 `hop_seed`가 나오도록 만든다.

## 2. 기능 개요

- 목적: 세션마다 다른 홉 패턴이 나오도록 `hop_seed`를 매번 새로 파생시킨다.
- 담당 모듈: `components/fhss_audio_adapter/fhss_audio_adapter.c`(파생 로직), `components/fhss_service/`(seed 교체 훅), `components/fhss_core/fhss_sync_packet.*`(전송 필드)
- 연산: HMAC-SHA256(RFC 2104), PSA Crypto API(`psa_hash_*`)로 직접 조립
- 동적 메모리: 사용하지 않음

## 3. 전체 데이터 흐름

이 기능(1단계)은 기존 홉 순서 생성 알고리즘(2단계, [fhss_hop_sequence_feature.md](fhss_hop_sequence_feature.md) 참고)의 **입력값을 매번 다르게 공급하는 역할**이다. 2단계 자체는 이 기능으로 인해 변경되지 않는다.

```text
[1단계 — 이 문서]                         [2단계 — 기존 로직, 무변경]

secret_seed(고정, 양쪽 공유)
public_seed(세션마다 새로 생성)
        ↓
  HMAC-SHA256 조합
        ↓
    hop_seed  ─────────────────→  xorshift32 + Fisher-Yates 셔플
                                          ↓
                                   채널 permutation table
                                          ↓
                                   slot_number → RF 채널
```

## 4. HMAC-SHA256 계산 상세

`secret_seed`는 4바이트로 SHA-256 블록 크기(64바이트)보다 짧으므로, RFC 2104에 따라 별도 해싱 없이 그대로 제로패딩한 것이 키(K')가 된다.

```text
message[4] = public_seed (big-endian 4바이트)
K'         = secret_seed(4바이트)를 64바이트로 제로패딩
ipad       = 0x36을 64바이트 반복 후 K'로 XOR
opad       = 0x5C를 64바이트 반복 후 K'로 XOR
inner      = SHA256(ipad || message)
outer      = SHA256(opad || inner)
hop_seed   = outer[0:4]  // 다이제스트 앞 4바이트를 big-endian uint32로 해석
```

### 코드 (`fhss_audio_adapter.c`)

```c
static uint32_t derive_session_hop_seed(uint32_t public_seed, void *context)
{
    const uint8_t message[4] = {
        (uint8_t)((public_seed >> 24) & 0xFFU),
        (uint8_t)((public_seed >> 16) & 0xFFU),
        (uint8_t)((public_seed >> 8) & 0xFFU),
        (uint8_t)(public_seed & 0xFFU),
    };

    uint8_t ipad[64], opad[64];
    memset(ipad, 0x36, sizeof(ipad));
    memset(opad, 0x5C, sizeof(opad));
    for (size_t i = 0; i < sizeof(s_secret_seed); ++i) {
        ipad[i] ^= s_secret_seed[i];
        opad[i] ^= s_secret_seed[i];
    }

    uint8_t inner_digest[32], outer_digest[32];
    sha256_compute(ipad, sizeof(ipad), message, sizeof(message), inner_digest);
    sha256_compute(opad, sizeof(opad), inner_digest, sizeof(inner_digest), outer_digest);

    return ((uint32_t)outer_digest[0] << 24) |
           ((uint32_t)outer_digest[1] << 16) |
           ((uint32_t)outer_digest[2] << 8) |
           (uint32_t)outer_digest[3];
}
```

`sha256_compute()`는 `psa_hash_setup/update/finish`(PSA Crypto API)로 SHA-256 한 번을 계산하는 헬퍼다. 이 ESP-IDF(mbedtls 4.x/TF-PSA-Crypto) 빌드에서는 `mbedtls_sha256()` 같은 옛 API가 "private" 식별자로 막혀 있어 직접 호출할 수 없다.

## 5. 왜 XOR/덧셈이 아니라 HMAC인가

XOR/덧셈처럼 가역 연산으로 조합하면, 공격자가 어떤 경로로든 한 세션의 `session_seed`(=hop_seed)를 알아낼 경우 `secret_seed = session_seed XOR public_seed`로 즉시 역산할 수 있다. 이후 모든 세션이 영구히 노출된다. HMAC은 출력에서 key(=secret_seed)를 역산할 수 없는 일방향 함수이므로 이 위험이 없다.

## 6. 실행 시점과 세션 판별

| 시점 | 동작 |
|---|---|
| TX, PTT 시작(`fhss_audio_adapter_begin_tx()`) | `public_seed = esp_random()` 생성 → `derive_session_hop_seed()` 호출 → `service.config.hop_seed`/`.public_seed`에 대입 후 TX 역할 전환 |
| RX, SYNC 패킷 수신(`fhss_service.c`의 `receive_one()`) | 수신한 `public_seed`가 이전과 다르면(=새 세션) 같은 함수로 재파생 → `fhss_hop_sequence_init_seeded()`로 hop_sequence 즉석 재구성 |

RX가 아직 세션을 모르는 SEARCHING 상태에서도 재파생이 즉시 이뤄지는 이유: 랑데부 채널(hop_sequence의 index 0)은 seed와 무관하게 항상 고정이라, 첫 SYNC 수신 시점에 hop_sequence가 구세션 기준이어도 문제가 없다. 이후 slot 1부터 새 seed 기준으로 정확히 계산된다.

## 7. 와이어 포맷 변경

`public_seed`를 SYNC 패킷에 평문으로 실어 보내야 하므로 패킷 포맷이 바뀌었다.

| 필드 | 기존(버전 1) | 변경(버전 2) |
|---|---|---|
| 길이 | 13바이트 | 17바이트 |
| 추가 필드 | — | `public_seed`(4바이트, offset 13) |

## 8. 공개 인터페이스 / 설정 필드

```c
// fhss_service.h
typedef uint32_t (*fhss_service_derive_hop_seed_callback_t)(
    uint32_t public_seed,
    void *context
);

typedef struct {
    ...
    uint32_t public_seed;
    fhss_service_derive_hop_seed_callback_t derive_hop_seed;
    ...
} fhss_service_config_t;
```

`derive_hop_seed`가 `NULL`이면(콜백 미등록) 기존처럼 `hop_seed`가 고정값으로 유지되며 재파생 로직 자체가 동작하지 않는다 — 이 기능을 쓰지 않는 다른 서비스 인스턴스와 호환된다.

## 9. 현재 한계 — `secret_seed`가 소스 상수

`secret_seed`는 지금 `fhss_audio_adapter.c`에 하드코딩된 4바이트 상수(ASCII "KCCI")다. 소스 코드가 노출되면 사실상 공개 값과 다름없어, 이 구조가 주는 도청 방지 효과는 아직 없다 — 순수하게 **세션마다 홉 패턴을 다르게 만드는** 목적만 달성한 상태다. 진짜 비밀로 만들려면 `secret_seed` 자체를 소스 밖으로 옮겨야 한다(아래 후속 작업 참고).

## 10. 변경 파일

| 파일 | 역할 |
|---|---|
| `components/fhss_core/include/fhss_sync_packet.h` | `public_seed` 필드 추가, 버전/길이 상수 변경 |
| `components/fhss_core/fhss_sync_packet.c` | encode/decode에 `public_seed` 반영 |
| `components/fhss_service/include/fhss_service.h` | `public_seed`, `derive_hop_seed` 콜백 타입/필드 추가 |
| `components/fhss_service/fhss_service.c` | `send_sync()`가 `public_seed` 포함, `receive_one()`이 재파생 훅 호출 |
| `components/fhss_audio_adapter/fhss_audio_adapter.c` | `secret_seed` 상수, `derive_session_hop_seed()`, `sha256_compute()`, `begin_tx()`에서 `public_seed` 생성 |
| `components/fhss_audio_adapter/CMakeLists.txt` | `mbedtls`(PSA Crypto API 제공) 의존성 추가 |

## 11. 검증 결과

| 항목 | 결과 |
|---|---|
| 전체 ESP-IDF 빌드 | 성공 |
| 실기기 2대 handshake | 성공 — `hop_seed re-derived: public_seed=...` 로그로 재파생 시점 확인 |
| 세션별 홉 패턴 상이 여부 | 확인 완료(매 세션 `public_seed`가 달라 `hop_seed`도 매번 다름) |
| 슬롯 300ms 트래킹 정확도 | 정상(오차 0~50us 수준) |

## 12. 후속 작업

1. `secret_seed`를 소스 상수에서 NVS로 이전 — 조합 로직(HMAC-SHA256)은 그대로 재사용, 키 소스만 교체
2. 더 나아가 eFuse 읽기보호 블록 + `esp_hmac_calculate()` HW 주변장치로 이전 — 펌웨어 자신도 raw `secret_seed`를 읽을 수 없게 만들어 진짜 도청 방지 효과 확보
3. 팀의 다른 접근(OTA로 seed를 통째로 배포해 NVS에 저장하는 `fhss_config_store` 방식, `origin/fix/fhss-ota-sync-recovery` 등 최신 develop 계열)과 설계 방향이 다르므로, 병합 전 어느 쪽을 채택할지 결정 필요

## 13. Git 정보

- 브랜치: `feature/rf-audio-core-pinning`(HMAC 파생 자체는 `feature/fhss-hop-seed-hmac`에서 작업 후 병합)
- 기준 브랜치: `feature/gateway-discovery-sync`
- 원격 미publish 상태 — 로컬에만 존재
