# PGRazor native pilot

See [migration gates and build instructions](../../docs/NATIVE_MIGRATION.md).

The optional `libpgrazor-core.so` statically links Dobby at commit
`0932d69c320e786672361ab53825ba8f4245e9d3`. `native_build.py` pins the archive
SHA-256 and verifies extracted sources on every build. Newer upstream revisions
were evaluated but failed the Android build; this pin is a tested starting point,
not a statement that it is the newest or universally compatible version.

Only `JNI_OnLoad` and `pgrazor_native_api` are exported. Java uses RegisterNatives
for `NativeCore.apiVersion()` and `policySelfcheck()`. Gameplay resolves the C
function table documented in `pgrazor_native_api.h`: version and size followed by
install, update, sample, and selftest pointers (40 bytes on both supported ABIs).
Version 2 preserves those entries and adds `query_feature` (48 bytes total).
Feature tables isolate throw grading, Adventure Sync, catch presentation,
transfer decisions, AR+ metadata, Rocket minigames, Always Hit, catch animation,
incubation, buddy feeding, map-tap capture, quick-spin requests and PokéStop camera transitions. Their headers document each ABI; they are
internal tables, not additional exported symbols.

The spawn hook preserves the original double(pointer,pointer) signature. A
`CLOCK_BOOTTIME` lease, bounded to 2500 ms and including device suspend, controls
the radius floor. Disabling expires
the lease immediately; the trampoline and library remain loaded for process
lifetime. The first concurrent callback waits for original-pointer publication;
subsequent calls use the published pointer without locking. Native patch failure
does not fall back to a second engine on the same target.

`test_runner.cpp` is a standalone Android executable, never included in the app.
It exercises original chaining, finite-radius policy, invalid input, disable,
expiry, duplicate install and telemetry. The separate explicit selftest uses only
an own function; neither test proves correctness of the game's target ABI.

Compile a focused harness with `python pgrazor/native_harness.py map_tap`.
To run it against a reviewed core, add `--serial <device>` and, if needed,
`--adb <path>` or `--library <core.so>`. Select `--abi arm64-v8a` for a physical
ARM64 device; the default is x86_64. The runner checks the device shell architecture,
executes a separate test process, and writes hashes/results under
`build/native/harness`. It does not install or attach to the game.

Metadata resolution and target validation still use the existing gameplay runtime.
A successful compiler run or host JNI test is not evidence that translated ARM
hooking or the production game callback worked. See the migration document for
the current device validation status.


The account controller has a separate fork-isolated harness in `tests/accounts_test.cpp`.
It includes the production controller to inspect cancellation and replay state. Compile it
with the NDK C++ driver, `-O2 -std=c++17 -pthread -static-libstdc++`, the Dobby include
directory, and `-DPGR_ACCOUNTS_REAL_DOBBY=1`; link `tests/accounts_dobby_bridge.cpp`,
`hook_engine.cpp`, the matching ABI's `libdobby.a`, `-llog`, and `-ldl`. The explicit
bridge connects the fixture's C hook stub to the production C++ hook engine; no linker
`--wrap` flag is used. Run the executable in a separate device process. It uses synthetic
authentication objects, never signs out of the game, and is not included in the APK.
Omitting the define and bridge/engine/library gives the original stub-only mode.


Feature 24 (`catch_completion`) owns direct-catch identity validation and the
final supplied dismissal Action. It adds no hooks and holds no objects beyond its
synchronous calls. The summary/canvas fallback and transfer orchestration stay in
the host. Run `python pgrazor/native_harness.py catch_completion` for its isolated
production-library harness; device validation is tracked in `docs/NATIVE_MIGRATION.md`.
