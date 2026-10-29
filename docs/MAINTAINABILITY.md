# Maintainability review

Current ownership after the compiled production cutover: native hosts and compiled Java/ART bridges replace the production JavaScript factories described in the historical review below. Add production behavior to its native host/leaf contract, preserve typed transport and Unity-thread ownership, and exercise the actual native boundary fixtures. Frida scripts and bundle composition remain only for explicitly selected research probes. Neutral JSON metadata declarations feed native schema generators, so production generation does not depend on retired JavaScript. Source archives require either tracked source or the explicit audited `--include-working-tree` allowlist; ignored inputs, credentials and diagnostic evidence remain excluded.

The dated review and counts below describe earlier work, not the current production architecture. See [ARCHITECTURE.md](ARCHITECTURE.md) and [BUILDING.md](BUILDING.md) for current ownership and build selection.

Reviewed after v0.1.2, October 2026. Two Claude reviews covered Java/UI and runtime/build tooling; the coordinator verified findings and implemented the first cleanup. This is a structural review, not a security audit or proof of native runtime compatibility.

## Findings and first cleanup

| Finding | Action |
| --- | --- |
| CI omitted eleven Node suites and the Java Control policy/settings runner | Discover all `test_*.py` and `*-test.cjs` suites, repair obsolete fixtures, and wire the orphaned cooldown test into its Java runner |
| Bootstrap mixed feature coordination with GPS permission/delivery internals | Extract `runtime/location.js` with explicit dependencies and owned cleanup; test its API directly |
| Packaging composed scripts ad hoc | Add `runtime/bundles.json` and a shared deterministic composer; validate packaged bytes/hash and syntax-check complete bundles |
| Quick-summary failure and revocation repeated the same policy reset list | Share `resetSummaryPolicy()` while retaining each caller's distinct error handling |
| `Control` repeated the foreground/backend/connection condition across features | Use a private `hostLive()` predicate; keep exceptional gates and the public bridge API intact |
| Architecture omitted current modules/test runners and described VGP as map-only | Update the source/lifecycle/test maps and contributor guidance |
| Restored quick-ball coverage exposed IV work bypassing animation cleanup on a clock error | Keep speed restoration reachable after IV failure and allow stopping IV labels without querying the clock |

No settings keys, package identity, signing identity, native offsets, gameplay rules, or released asset paths are intentionally changed by this cleanup. Modules are bundled at build time so startup does not gain another runtime loader.

Validation for this pass: `tools/check.py` passed with 72 Python tests, 27 Node suites, the Java core checks, and script syntax checks. Full `pgrazor/build.py` compilation and saved-place validation also passed. No device installation or gameplay validation was performed. Both Claude review workers and the Java implementation worker completed; a JavaScript fixture worker timed out without edits, and the coordinator completed those repairs and the clock-error cleanup fix.

## Module cleanup

The completed follow-up uses these ownership boundaries:

| Boundary | Change |
| --- | --- |
| Native feature state machines | Separate factories for berries, buddy, gifts, incubation, spawn, items, battles, Rocket/minigame and VGP; metadata validation and Unity scheduling remain in `quick-close` |
| Shared native helpers | Explicit boxed-item and promise helpers; lazy inventory-state access preserves initialization order |
| Native test fixtures | Invoke the moved feature APIs through the bundle manifest instead of slicing source by comments |
| VGP and catch outcomes | Dedicated runtime controllers, with real Java transition and toast-cancellation tests; `Control` remains the public bridge |
| Map rendering and retrieval | Separate tile source/cache and POI layer from route editing, gestures and selected destination |
| Menu ownership | `Pad` retains window/overlay/joystick lifecycle; `SettingsPanel` owns named section builders/refresh binders and dialogs; `SettingsUi` shares styling |
| Settings persistence | Ordered, coalesced background writes; explicit flush for backup/import and reload; bounded background flush; failed reload retains live settings |
| Packaging evidence | Record the exact composed script hash and each source hash in the build receipt |

Independent native comparison found 79 moved named functions with unchanged parsed implementations after normalizing explicit dependency access. Scope checks found no unintended free variables. This checks the move, not the correctness of game-specific metadata.

Independent review also caught a failed-flush reload reading stale disk settings. The reload now aborts, and a real-Java regression verifies it alongside export/import ordering and legacy berry migration.

A final Claude review confirmed the settings lock/flush design and identified an existing map scheduling race. Tile work now remains pending until its main-thread completion callback clears ownership, preventing an old callback from losing track of a newer task. Removed queued bitmap callbacks remain eligible for garbage collection on supported Android versions; no shared cached bitmap is recycled while a view may still use it.

This pass is complete. The coordinator verified every original menu string and Control action remains represented, compiled the Android UI, and added item-dialog polling cleanup on panel detach. The public Control signatures and saved setting key set are unchanged.

Final automated validation: 73 Python tests, all 27 Node suites, Java core/policy checks, individual and composed script syntax checks, and a full Android compilation passed. The counts include the ordered writer runner; the Control runner also executes VGP transitions and settings export/import/reload regressions. The non-root APK set also packaged successfully; the coordinator verified the embedded script bytes and source hashes against the current tree and confirmed all extracted Java owners are present in DEX. No device installation or gameplay/UI rendering validation was performed.

Claude completed the runtime-controller, settings-integration and menu extractions plus a final read-only review. The map worker timed out after writing its extraction without a final report; the coordinator finished and verified that work. Worker reports and verification logs remain local under ignored `.artifacts/`.

| Main entry point | Before | After |
| --- | ---: | ---: |
| `diagnostics/quick-close.js` | 2,934 lines | 1,882 lines |
| `embedded-agent.js` | 816 | 677 |
| `Pad.java` | 739 | 389 |
| `WorldMapDialog.java` | 969 | 576 |

Code moved into explicit owners rather than disappearing. These sizes describe reduced responsibility in entry points, not reduced total implementation size.

## Where future changes belong

- Add feature-specific native behavior to its factory. Keep version-specific resolution, shared lifecycle, tick ordering and ownership registries centralized until there is a clear independent boundary.
- Keep `Control` bridge signatures and persisted keys stable. The enabled/readiness flags remain alongside the other bridge gates; VGP status, guards, markers and notifications have a separate owner.
- Keep map interaction changes separate from tile/cache or marker changes. Those layers now communicate through explicit interfaces.
- `LiveFeedDialog` remains a large UI orchestrator with existing client/data/filter boundaries. Split it when a concrete feature change benefits from a smaller UI owner; line count alone is not a reason to rewrite it.
- A few fixtures still slice unmoved presentation, IV, or bridge code. Replace those fixtures with direct APIs before extracting those regions. The moved feature fixtures no longer need their old source boundaries.

## Constraints and hygiene

- Native signatures and offsets are tied to the supported game build. Missing metadata must continue to disable the affected feature. Mock tests cannot verify those contracts against a new game binary.
- Preserve the 1,500 ms startup grace and GPS main-thread installation. Do not replace guarded native lifecycle dispatchers with virtual Java hooks that previously crashed on ART.
- `diagnostics/` contains shipped functionality, not just disposable research scripts. Renaming it is lower priority than separating responsibilities and would require packaging/reference updates.
- Local game inputs, APK outputs, research captures and worker reports are ignored, not source clutter. Preserve unique evidence and original inputs. Generated-artifact cleanup belongs to `cleanup.ps1` and its retention pins; deleting old directories by age is not a maintenance strategy.
- Local SDK defaults and CI SDK pins differ. Reproduce CI with its documented environment variables before attributing build differences to a refactor.

Follow-up changes should state their boundary, preserved contracts, automated checks and device observations. Avoid combining a module move, formatting rewrite and gameplay change in the same patch.

## Device regression follow-up

Initial user testing found VGP, Quick Catch dismissal, catching berries, transfer and spin unavailable together. The native extraction had left `vgpField` private to the promise module while the combined installer still referenced it. Individual factory tests and byte/hash checks did not exercise this integration boundary. The helper is now exported and wired explicitly. `feature-install-test.cjs` executes the complete composed installer with real factories, checks every injected dependency, verifies deferred inventory initialization, and requires metadata readiness. It reproduced the ReferenceError before the fix. Device gameplay results remain separate from this mocked installation check.

Device follow-up confirmed the old run ended with `ReferenceError: vgpField is not defined`. After installing the fix, live logs report shared metadata readiness, VGP readiness, berry/impact readiness and Quick Ball readiness. The user subsequently reported that everything seems to be working. This is user acceptance feedback, not an exhaustive automated gameplay test.

## Independent review before commit

Two further read-only Claude reviews covered the complete staged native/runtime/build and Java/UI/persistence changes. Neither found a remaining behavioral regression. The coordinator verified their observations against the current code and the original revision. The startup delay and buddy sampling mentioned as potentially separate work were already present in the original revision; their fixture changes repair stale tests.

The installer regression test now compares supplied dependency names with each real factory's declared contract, catching entirely omitted dependencies as well as undefined values. The synchronous installation order remains important: making installation asynchronous would require revisiting tick initialization. Background settings flush intentionally waits up to one second for pending writes; the inherited interrupted-cache-read behavior can discard a cached tile and cause a later re-fetch.

Validation passed in both the working tree and a separately extracted Git-visible source archive: 73 Python tests, 28 Node suites, Java policy checks, and individual/composed JavaScript syntax checks. The strengthened installer test also passes. Android UI rendering and native game contracts still require device observations; these static reviews and mocked tests do not replace them. Worker reports and detailed receipts stay local.
