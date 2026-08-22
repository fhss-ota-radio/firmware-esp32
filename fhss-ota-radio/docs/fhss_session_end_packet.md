# FHSS 음성 세션 END 패킷

## 목적

PTT를 놓은 사실을 송신기가 명시적으로 알린다. 수신기는 1초 무음 timeout이나
연속 SYNC MISS를 기다리지 않고 `RX_AUDIO`를 정상 종료하고 공통 시작 채널로
복귀한다.

## 전송 절차

1. 남은 Speex frame을 AUDIO 패킷으로 전송한다.
2. `session_id`, 마지막 AUDIO sequence, 종료 이유가 든 END 패킷을 3회 큐에 넣는다.
3. 송신 큐와 CC1101 전송이 비워질 때까지 기다린다.
4. 송신 보드도 RX 대기 역할로 돌아간다.

END는 별도 packet type `0x03`인 9-byte 제어 패킷이다. AUDIO packet type
`0x02`의 형식은 바꾸지 않았다. 전송할 오디오 frame이 하나도 없는 짧은 PTT에도
END를 표현할 수 있다는 점이 기존 `END_OF_TALKSPURT` flag와의 차이다.

## 수신 절차

1. 올바른 END를 받으면 동일 `session_id/final_sequence` 중복 여부를 확인한다.
2. 최초 패킷만 adapter event `TALKSPURT_ENDED`로 상위 FSM에 전달한다.
3. 상위 FSM은 이를 `FSM_EVENT_RX_DONE`으로 변환해 `RX_AUDIO → MENU_COMM`으로 전이한다.
4. FHSS service는 scheduler 기준, MISS/RECOVERY 카운터를 초기화하고 rendezvous
   channel로 즉시 복귀한다.
5. 반복 송신된 동일 END는 앱 이벤트를 다시 발생시키지 않는다.

정상 종료에는 `SYNC_LOST`를 사용하지 않는다. `SYNC_LOST`는 END 자체가 손실되고
실제로 호핑 추종까지 잃었을 때 사용하는 비정상 복구 경로로 남긴다.

## 실기기 판정 로그

TX에서 PTT를 놓았을 때:

```text
TALKSPURT_END TX: session=... final_sequence=... copies=3
TX session ended: ... RX standby resumed
```

RX에서:

```text
TALKSPURT_END RX: session=... final_sequence=...
peer talkspurt ended; rendezvous RX resumed on channel=1
fsm: RX_AUDIO -> MENU_COMM
```

위 정상 종료 직후 `RECOVERY`, `SYNC_LOST`, `ERROR`가 출력되지 않아야 한다.

## 현재 한계

- END 3회는 ACK가 아닌 단순 반복 전송이다. 세 패킷이 모두 손실되면 기존 무음
  timeout/동기 상실 복구가 fallback으로 동작한다.
- 현재 AUDIO header에는 `session_id`가 없다. 여러 송신자가 겹치는 환경까지
  확장할 때는 AUDIO protocol v2에 session/source 식별자를 포함해야 한다.
- 실제 END 도달률과 종료 지연은 두 보드 실험으로 측정해야 한다.
