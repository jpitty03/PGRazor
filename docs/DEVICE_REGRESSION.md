# Device regression testing

The first runner covers native runtime startup and authenticated-session evidence across cold launches. It works through ADB on either Root or patched installations, with no root grant required. It does not yet automate gameplay interactions or prove that the map rendered correctly. The report lists unimplemented gameplay scenarios as **skipped**, not passed.

## Run

Enable USB debugging and authorize your computer on the phone, or connect to the intended MuMu instance's ADB port. Always supply an exact serial. There is no automatic device selection, installation, credential entry, or app-data clearing.

Preview the actions without accessing a device:

```powershell
py -3.12 tools/regression/run.py --serial 127.0.0.1:16416
```

Run two cold launches on MuMu (use `--adb PATH` if ADB is not on PATH):

```powershell
py -3.12 tools/regression/run.py --serial 127.0.0.1:16416 --execute
```

For a phone, substitute its exact `adb devices` serial. `--execute` stops and opens the game on that device, so finish any encounter first. Existing game automation settings remain active; prepare a stationary test account with automation disabled before running. The runner does not alter or restore settings.

For a session-renewal observation, idle on the home screen for an hour between cold launches:

```powershell
py -3.12 tools/regression/run.py --serial YOUR_SERIAL --execute --cycles 2 --idle-seconds 3600
```

This checks whether a session remains usable after that interval; it does not force token expiry or prove renewal occurred. The game remains open after the last cycle. Stop the host command with Ctrl+C to write an interrupted report.

## Evidence and results

Each run creates `.artifacts/device-regression/<timestamp-id>/report.json` and `report.md`, plus per-cycle screenshots and fixed event labels. Screenshots may contain your account name or game information: keep receipts private. Raw logcat and authentication payloads are not saved. The runner does not clear the device's logcat buffer.

- **Passed:** the required positive evidence was observed for this launch's process.
- **Failed:** a native startup failure or process replacement/exit was observed.
- **Blocked:** device unavailable, observation incomplete, sign-in required, or required instrumentation missing. Inspect the screenshot; a timeout alone cannot identify the cause.
- **Skipped:** the gameplay test adapter is not implemented.

Exit codes: `0` smoke checks passed, `1` a check failed, `2` checks were blocked or the run was interrupted. A smoke pass does not imply the skipped gameplay scenarios passed. Execution stops after an unsuccessful cycle.

Runtime readiness requires both `native-bootstrap-ready` and `native-scheduler-ready`. Authentication requires the new `account-session-verified` event emitted after the game's identity matches the selected PGRazor profile. That event contains no account identifier or trainer name. Older installed builds lack it and will correctly report authentication as blocked; install a build containing this branch's change before expecting that check to pass. Identity verification is distinct from rendered-map verification.

Host fixtures run in normal CI through `tools/check.py`, or directly:

```powershell
py -3.12 -m unittest discover -s pgrazor/test -p test_device_regression.py
```

## Next adapters

Add controlled settings readback and restoration, then catching/spinning with inventory deltas and game-result events. Account switching needs known test profiles; routes/combat/cutscenes need suitable live targets. Transfer, travel and resource-consuming tests need explicit per-run opt-in and disposable test data. Keep missing prerequisites distinct from feature failures, and keep screenshots alongside positive result evidence. Do not add coordinate-only scripts that report a toggle click as a working feature.
