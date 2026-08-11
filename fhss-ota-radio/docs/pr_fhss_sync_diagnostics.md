# feat: add FHSS synchronization diagnostics

## Summary

- Add reusable FHSS diagnostics counters and snapshots.
- Classify RX timeout, CRC failure, radio error, and SYNC processing error separately.
- Report aggregate timing error and per-channel reception statistics periodically.
- Expose a mutex-protected diagnostics snapshot API for future UI integration.

## Changes

- Added `fhss_diagnostics` module under `components/fhss_service`.
- Added total and per-channel valid/CRC-fail/timeout counters.
- Added SYNC acquired/lost counters.
- Added timing error minimum, maximum, sum, sample count, and last-valid timestamp.
- Added `fhss_service_get_diagnostics()`.
- Added configurable `diagnostics_interval_ms`; `main` currently uses 5 seconds.
- Kept radio/controller errors separate from RF MISS accounting.

## Test

- [x] `ninja -C build`
- [x] COM5 RX flash and image verification
- [x] Periodic diagnostic logging
- [x] Synchronized hopping remains in TRACKING
- [x] All three channels accumulate balanced valid counts
- [x] No CRC failure or SYNC_LOST during the 20-second smoke test

Observed result:

```text
DIAG state=TRACKING valid=118 crc_fail=0 timeout=4 acquired=1 lost=0 timing_us[min/avg/max]=-6/-1/5 last_valid_age_ms=9
DIAG channel=0  valid=39 crc_fail=0 timeout=1
DIAG channel=10 valid=39 crc_fail=0 timeout=2
DIAG channel=20 valid=40 crc_fail=0 timeout=1
```

Firmware size:

```text
fhss-ota-radio.bin: 0x4fd90
smallest app partition free: 69%
```

## Notes

- The four timeouts occurred during initial channel search and did not increase after TRACKING was acquired.
- This PR is stacked on `feature/fhss-radio-sync-integration`; use that branch as the PR base until PR #24 is merged.
- A longer measurement is still required before selecting a non-zero `sync_offset_us`.

