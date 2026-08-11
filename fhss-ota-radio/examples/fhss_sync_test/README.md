# FHSS synchronized hopping test application

This standalone ESP-IDF application contains the fixed-role TX/RX hardware
test harness. It intentionally does not depend on or modify the product
application FSM in `main/fsm.*`.

The product FSM follows `docs/fsm-design.md`: a device changes between idle,
audio TX/RX, and OTA modes at runtime. The current `fhss_service` test harness
instead fixes one board as TX and one as RX, so it must remain an example until
session-based start/stop and role switching are implemented.

## Build

Run these commands from this directory:

```powershell
idf.py set-target esp32s3
idf.py build
```

Set `FHSS_TEST_ROLE` in `main/main.c` before flashing:

- COM3: `FHSS_TEST_ROLE_TX`
- COM5: `FHSS_TEST_ROLE_RX`

