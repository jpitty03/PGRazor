# PGRazor codebase map for AI agents

Read this before changing code. It is a navigation guide, not a substitute for the
files it points at: paths and line numbers are the source of truth. Project root is
the repository root; `pgrazor/` is the Android/native product, `website/` is the
separate GitHub Pages site.

## 1. What this project is

PGRazor injects an in-game menu, joystick, encounter controls and optional automation
into a user-supplied, Niantic-signed **ARM64 Pokémon GO 0.437.0** APK.

- `README.md` — user-facing overview and install links.
- `docs/ARCHITECTURE.md` — ownership boundaries and source map (**start here**).
- `docs/MAINTAINABILITY.md` — historical refactor notes; its JavaScript factory
  descriptions predate the compiled-production cutover. Treat as history.
- `docs/NATIVE_MIGRATION.md` — compiled-runtime feature inventory and verification status.
- `docs/BUILDING.md` — toolchain, setup, local build, signing and release process.
- `docs/GAME_UPDATES.md` — the game-version update procedure.
- `CONTRIBUTING.md` — rules every code change must obey.

## 2. The three runtime layers

1. **Java controls** — `pgrazor/src/dev/pgrazor/*.java`. Settings, menu, overlays,
   map UI, lifecycle, location state, notifications. `Control.java` is the persisted
   state and public static bridge API; do not change its public signatures or saved
   keys casually.
2. **Compiled Java/ART bridges** — `pgrazor/art/` and Java bridge files. Own game
   lifecycle, permissions and location. `NativeRuntimeTransport.java` exchanges
   typed settings/events with native feature52.
3. **C++ runtime** — `pgrazor/native/*.cpp|h`. `runtime_host.cpp`/`runtime_bootstrap.cpp`
   drive hosts 60–71 plus catch-wait58 on the verified Unity thread. Native metadata,
   observers and short foreground/map leases gate gameplay.

`nonroot.py` injects these into the original game; `release_apk.py` merges the
library-only ARM64 split into one signed `PGRazor.apk`. Game APKs are inputs, never
source. Production packages contain no Frida runtime and no PGRazor JavaScript;
`runtime/`, `diagnostics/`, `embedded-agent.js` and `script_bundle.py` are only for
explicit `--research-runtime` builds.

## 3. Hard rules (from CONTRIBUTING.md / ARCHITECTURE.md)

- Keep internal Java package names (`dev.pgrazor`), serialized property names,
  signing identity and asset paths stable unless implementing an explicit migration.
- Keep the public `Control` bridge API and all saved settings keys stable.
- Native fields, methods, byte signatures and enums are game-version-specific.
  Missing contracts must disable features, never authorize unknown writes.
- Preserve the 1500 ms native startup grace, foreground/map leases, GC-root
  ownership, cancellation and error handling.
- Never use an automated test to spend balls, transfer live Pokémon or change a
  user's inventory.
- Never commit game inputs, signing keys, private logs, account data or coordinates.
- One focused change per PR. Do not bump versions or tags in a feature PR.
- Automated tests do not validate game offsets, Android rendering or live gameplay;
  report automated results, compilation and device observations separately.

## 4. Local environment on this machine

- Repo: `D:\Projectos\Android\Pokemon Go\PGRazor` (branch `main`).
- Python: `.venv\` (3.12, `cmake==4.4.4`, `ninja==1.13.2`). Invoke as
  `.venv\Scripts\python.exe`.
- Node: website `npm ci` already run in `website\`.
- Android SDK: `D:\Android\Sdk` — platforms `android-36`, build-tools `36.0.0`,
  sources `android-36`, NDKs `27.3.13750724` (gameplay) and `29.0.14206865` (ART).
- User env vars are set for `ANDROID_HOME`, `ANDROID_SDK_ROOT`, `ANDROID_NDK_HOME`,
  `PGRAZOR_ART_NDK`, `PGRAZOR_NATIVE_DEPS`, `PGRAZOR_ANDROID_PLATFORM`,
  `PGRAZOR_BUILD_TOOLS`.
- **PATH trap:** the machine's first `git` is devkitPro MSYS2 (reports `/d/...`
  paths) and its first `cmake` is 4.0.2. Both break the repo's provenance/version
  checks. Dot-source the gitignored local helper in every shell:
  `.\.local\env.ps1` (puts `.venv\Scripts`, Git for Windows, SDK build-tools and
  JDK 17 first).
- Pinned native sources are cached in `pgrazor\build\native\deps` and verified via
  `tools\prepare_native.py` (use `--download` once only).
- DEX tool jars are cached in `.artifacts\native-migration\dex-tools` via
  `dex_bridges.dependencies(download=True)`.
- Game input lives at `input\com.nianticlabs.pokemongo_0.437.0.apkm` (gitignored,
  SHA-256 `5be1ff37…afebdd3`).

## 5. Common commands

```powershell
# activate correct toolchain first
. .\.local\env.ps1

# device-free gate used by CI (must be exit 0)
.\.venv\Scripts\python.exe -B tools\check.py

# Android controls compilation (required for UI changes)
.\.venv\Scripts\python.exe -B pgrazor\build.py

# full local build against the pinned game input
.\.venv\Scripts\python.exe -B pgrazor\nonroot.py --apk "input\com.nianticlabs.pokemongo_0.437.0.apkm" --download-runtime
.\.venv\Scripts\python.exe -B pgrazor\release_apk.py --run "pgrazor\build\nonroot\<RUN>" --output pgrazor\build\release

# native prerequisite verification
.\.venv\Scripts\python.exe -B tools\prepare_native.py
.\.venv\Scripts\python.exe -B pgrazor\art_build.py --check

# website
cd website; npm run build; npx playwright install chromium; npm test
```

Known harmless warning: `cleanup.ps1` throws `Invalid retention configuration` on a
fresh checkout because `Get-Property` returns `$null` for the empty `pinnedRuns`
array. Packaging still succeeds; only obsolete-APK pruning is skipped.

## 6. Where to add a new user-facing option

### 6.1 How a setting reaches gameplay and comes back

```text
SettingsPanel / Pad / *Preferences
  → Control.java static getters/setters (persisted by SettingsWriter)
    → NativeRuntimeTransport.java buildSnapshot()  [50 ms host daemon, not Unity thread]
      → JNI NativeExchange → feature52 runtime_transport.{h,cpp}
        → typed get_bool/get_int/get_string/take_pulse consumed by leaves and hosts
        → emit(opcode,...) events drained by Java and dispatched to Control setters
Native hooks resolve addresses from generated game_bindings.h ← schema/game/<version>.json
Runtime orchestration: feature54 bootstrap → feature48 scheduler → feature70 manager → hosts 60–71 / catch-wait58
```

**Two different meanings of "feature" exist — do not confuse them:**

- *Transport booleans* are settings/readiness flags. The canonical ordered list is
  `NativeRuntimeTransport.java:42-51`; native code reads them by **positional index**
  (often literal numbers with comments, e.g. bool 3 `active`, 38 `onMap`, 40
  `gymRepeatEnabled`). There is no native name enum.
- *Native feature IDs* are registered in `pgrazor_native.cpp:307-382`
  (`query_feature`): 52 transport, 54 bootstrap, 48 scheduler, 58 catch-wait,
  59 camera metadata, 60–71 hosts, 47/53/56 metadata. Only `JNI_OnLoad` and
  `pgrazor_native_api` are exported (`pgrazor.map`).

### 6.2 Java controls — key files

| Concern | File |
| --- | --- |
| Persisted state, readiness gates, public static bridge | `pgrazor/src/dev/pgrazor/Control.java` |
| Ordered/coalesced writes | `pgrazor/src/dev/pgrazor/SettingsWriter.java` |
| Menu body: section builders, refresh binders, dialogs | `pgrazor/src/dev/pgrazor/SettingsPanel.java` |
| Attachment, joystick, launcher, overlays | `pgrazor/src/dev/pgrazor/Pad.java` |
| "R" quick menu | `pgrazor/src/dev/pgrazor/QuickMenu.java` |
| Shared palette / recursive theming | `pgrazor/src/dev/pgrazor/SettingsUi.java` |
| Typed native snapshot/events | `pgrazor/src/dev/pgrazor/NativeRuntimeTransport.java` |
| Backup JSON allow-list / validation | `pgrazor/src/dev/pgrazor/SettingsBackupRules.java`, `SettingsBackup.java` |

**Menu.** `SettingsPanel.Host` (lines 21-36) delegates overlays/navigation back to
`Pad`. `page()` (161-177) registers a titled page; the constructor (206-262) calls
the builders in fixed order (location, catching, Pokémon, Go Plus, battles, social,
automation, events, display, accounts). `switchRow()` (117-122) creates a labelled
switch. Theme handling is automatic through `SettingsUi.styleTree()`.
**Every new widget must be synced in `SettingsPanel.refresh()` (739-763) or it will
show stale state**; enable/disable rules belong in `refreshDependencies()` (765-782).
The "R" launcher is created in `Pad.java:195-197`; `QuickMenu` has a **append-only**
`CUSTOMIZABLE` array (line 42) and 12 rows with single-bit visibility flags. Adding
a quick-menu row needs no new property key, but raising the bit above 4096 requires
widening the guard in `Control.setQuickMenuVisible()` (1138), the load bound 8191
(1201) and the backup rule (line 41).

**Persistence.** Store: `pgrazor-settings-v1.properties` under
`AccountProfiles.file(app, ...)` (`Control.java:1172`), written with `AtomicFile`.
`loadSettings()` (1163-1296) parses keys with inline defaults and migrations
(legacy berry 1222-1227, transfer costume-bit 1239-1241, corrupt keep-list fail-safe
1246-1259). `settingsSnapshot()` (1346-1391) is the canonical persisted-key list;
`saveSettings()` (1339-1344) submits a snapshot to `SettingsWriter` (ordered,
coalesced, off-thread; sink never takes `stateLock`). Export/import flush before
touching the file (1311-1337); background flush is bounded to 1 s; abrupt death can
lose the newest unflushed change. There is no version key in the properties file —
evolution uses stable key names plus inline migrations. JSON backups are `version:1`.

**Bridge events.** `Control.event(kind, detail)` (1442-1530) fans native events into
module objects and status strings, then logs them. It runs on the native transport
daemon thread and must never touch Android Views; UI observes via `Pad.refresh()` /
`SettingsPanel.refresh()` on the main thread. Config strings such as
`smartCatchConfig()`, `gymAutomationConfig()`, `batteryConfig()` are pipelimited and
versioned by prefix; adding a bit to an existing config string is often cheaper than
extending the native frame.

### 6.3 Adding one Java-only boolean toggle (precedent pattern)

1. `Control.java` — add `private static volatile boolean myToggle;`, a public
   getter/setter (setter calls `saveSettings()` and optionally `event(...)`),
   `myToggle=Boolean.parseBoolean(p.getProperty("myToggle","false"));` in
   `loadSettings()`, and `p.setProperty("myToggle",String.valueOf(myToggle));` in
   `settingsSnapshot()`. Never rename an existing key; ints go through
   `boundedSetting()` (1297-1300) and destructive settings fail closed.
2. `SettingsBackupRules.java:8-9` — add the key to `SETTINGS` and `BOOLEANS`, or
   `SettingsBackup.snapshot()` silently omits it and `validate()` rejects a backup
   that contains it.
3. `SettingsPanel.java` — declare the `Switch`, create it with `switchRow()` in the
   right `buildXPage()`, set initial state, write through the `Control` setter in
   `OnCheckedChangeListener`, and sync it in `refresh()` (a new `refreshX()` helper
   must be added to `refresh()`'s call list).
4. If native must read it, follow 6.5 instead — adding a bool to `BOOL_READERS` is a
   coordinated wire-protocol change (`FRAME_VERSION` bump).
5. Tests — extend `ControlSettingsTest.java` (save/load + direct property check),
   `ControlSettingsReloadTest.java` (default, export, import override), and
   `SettingsBackupRulesTest.java` (valid/invalid values). These are already driven
   by `test_control_java.py` and `test_live_feed_java.py`; no new runner needed.
   Never add a standalone Java test that CI does not run. If you extract a new
   class, add it to the fixed source list in `test_control_java.py:244-252` (or stub
   it consistently) and prefer direct module fixtures over source slicing.

### 6.4 Native transport (feature 52) — key files

| Concern | File |
| --- | --- |
| Wire ABI, capacities, frame layout | `pgrazor/native/runtime_transport.h:40-96,121-217` |
| Decode, replay guard, getters, emit queue | `pgrazor/native/runtime_transport.cpp:25-141,248-399,451-592` |
| JNI shim | `pgrazor/native/runtime_transport_jni.cpp/.h` |
| Host snapshot writer, event decoder, cancel | `pgrazor/src/dev/pgrazor/NativeRuntimeTransport.java:41-87,239-268,308-409,431-470` |
| Research-only scalar adapter | `pgrazor/runtime/native-transport.js:3-55` (intentionally lags native-only additions) |
| Fixtures | `pgrazor/native/runtime_transport_test.cpp`, `pgrazor/test/NativeRuntimeTransportTest.java`, `pgrazor/test/test_native_runtime_transport_java.py`, `pgrazor/test/native-transport-test.cjs` |

The frame is strict: magic `PGRS`, `FRAME_VERSION`, size, flags, sequence, bool bytes
+ zero padding, ints, doubles, pulses, then 24 length-prefixed UTF-8 strings.
`parse_snapshot()` rejects malformed UTF-8; `api_exchange()` rejects equal/older
sequence numbers or regressing pulses with `ERR_REPLAY (-22)`; getters fail closed
with `PGR_TRANSPORT_STALE (-19)` on an expired `CLOCK_BOOTTIME` lease, except
`kGuardedBool[41]` entries which return 0 (readiness flags must not appear enabled
on a stale snapshot). `api_stop()` invalidates the snapshot, clears the queue and
bumps generation; Java also calls `Control.disconnect()` and cancels the timer.

Action opcodes have an exact scalar/string schema in
`runtime_transport.cpp:25-92` (60 actions today) mirrored by Java arity/domain
checks. Events are consumed before dispatch, with contiguous sequence and
generation checks so a native restart cannot replay gameplay actions.

### 6.5 Adding one native-backed boolean option (precedent: `gymRepeatEnabled`)

1. `Control.java` — add field, getter, setter (call `saveSettings()` + `event(...)`),
   persist the key in `loadSettings()` and `settingsSnapshot()`, add to
   `SettingsBackupRules.java:8-9` if exportable. Add UI row in `SettingsPanel.java`.
2. `NativeRuntimeTransport.java` — **append** the getter name to `BOOL_READERS`;
   bump `FRAME_VERSION` (21 → 22). Resolution is reflective against `Control`, so a
   missing/mistyped getter aborts construction (fail-closed).
3. `runtime_transport.h/.cpp` — bump `PGR_TRANSPORT_FRAME_VERSION` and
   `PGR_TRANSPORT_BOOLS` (41 → 42), update the version-history comment, append one
   `kGuardedBool` entry (`false` for a user preference, `true` for a readiness gate).
   A 42nd bool still fits the existing padded region, so offsets do not change.
   For a config string instead/also, bump `PGR_TRANSPORT_STRINGS` and append a
   `putString()` in `buildSnapshot()` (`NativeRuntimeTransport.java:360-376`).
4. Native consumer — read coherently: `sample(before)` → `get_bool(slot)` →
   `sample(after)` → require same `snapshot_seq`, no fault and fresh lease
   (pattern `account_host.cpp:1286-1326`, `gym_repeat.cpp:306-319`). If the hook
   consumes it, derive a bounded `CLOCK_BOOTTIME` lease (≤2500 ms).
   A truly new host additionally requires: header API + `runtime_host.h` array and
   `static_assert`, five switches in `runtime_host.cpp`, a `query_feature` case, and
   a `CMakeLists.txt` entry.
5. Hook addresses must come from generated `PGR_GAME_*` macros. Needing a new
   target means adding a binding to the version profile and regenerating — never
   inline an address (audited by `game_bindings.py:11-33`).
6. Tests — `native/<stem>_test.cpp` with a stub transport asserting the exact slot
   (compile with `python -B pgrazor/native_harness.py <stem>`, add `--serial` for
   device); append the name to `test_native_runtime_transport_java.py:17-41`
   (`specs`) and assert the slot in `NativeRuntimeTransportTest.java`; update
   parity assertions in `native-transport-test.cjs:59-64`; add persistence cases to
   `ControlSettingsTest.java`, `ControlSettingsReloadTest.java`,
   `SettingsBackupRulesTest.java`.

**Never hand-edit generated files:** `pgrazor/native/game_bindings.h`,
`catch_wait_schema.h`, `routes_schema.h`, `shared_gift_code.h`,
`native/schema/routes.json`, `native/schema/gifts.json`, golden digests in
`pgrazor/test/test_native_schemas.py:44,53`, or anything under `pgrazor/build/`.

### 6.6 Game-version updates — what survives and what must be re-applied

The per-version truth is a single profile JSON:
`pgrazor/native/schema/game/<version>.json`, selected by
`pgrazor/native/schema/game/current.json` (currently `0.437.0.json`). It holds the
game identity, four checksums, `modules` (catch-wait parameters), `bindings` and
`schemas` (routes/gifts). A bump copies the newest profile, changes values, flips
`current.json`, and regenerates. **All custom source survives automatically as long
as it references `PGR_GAME_<MODULE>_<KEY>` macros or stable code keys — but a newly
added binding entry survives only if the next profile is copied from the newest one.**

Generated from the profile (never edit by hand): `game_bindings.h`,
`catch_wait_schema.h`, `routes_schema.h`, `shared_gift_code.h`, `schema/routes.json`,
`schema/gifts.json`. Regenerate with
`python -B tools/game_update.py generate`, then `generate --check`, `audit`,
`python -B tools/check.py`. `game_update.py:159-198` computes all outputs before
writing any. `test_game_update.py:16-27` scans source for `PGR_GAME_*` references and
fails if the selected profile lacks any of them — the strongest update-proof guard.
Golden hashes in `test_game_update.py:133-140` and `test_native_schemas.py:25-54`
must be refreshed only after reviewing a regenerated diff.

Update workflow (`docs/GAME_UPDATES.md:19-29,84-100`):

```powershell
python -B tools/game_update.py verify-input path/to/game.apkm
python -B tools/game_update.py compare --old old-signatures.json --new new-signatures.json --output .artifacts/game-update/report.json
# copy newest profile to <new-version>.json; update values + 4 checksums + current.json
python -B tools/game_update.py generate
python -B tools/game_update.py generate --check
python -B tools/game_update.py audit
python -B tools/check.py
```

`compare` never auto-approves RVAs; its report is always `compatible: False` and
requires human review of renamed types, ABI/signature changes, field layouts,
patch sites and fingerprints. Only `input_sha256` is actively enforced at build
time (`private_input.py:35`); library/metadata/signer hashes are recorded evidence.

**Hand-maintained version-specific adapters — highest-risk files on every bump:**
`pgrazor/native/auto_raid_game_map.inc`, `auto_raid_game_catch.inc`,
`map_auto_throw_game.inc` (obfuscated names/offsets as literals, invisible to the
address audit), `gym_repeat.cpp`, `shundo.cpp` (hook arity/type changes),
`camera_metadata.cpp/.h`, `shared_metadata.h`, `shared_metadata_gifts.inc`,
`shared_metadata_map.inc`, `quickball_metadata.*`, plus fixtures
`catch_wait_test.cpp`, `camera_metadata_test.cpp`, `quickball_metadata_test.cpp`,
`routes_host_test.cpp`, `routes_metadata_test.cpp`, `interaction_host_test.cpp`.
The real 0.435.2 → 0.437.0 bump changed 567 binding values, 27 files, and required
source edits for a moved camera class, renamed gym enum, and a 4→5 argument hook —
proof that generation alone is not compatibility.

**Design an update-proof option:** put every version-specific value in one profile
entry with a stable key and reference only its macro from source; add that entry to
*every* later profile by copying the latest; prefer stable code keys
(`shared_gift_code.h`/routes schema with `pinned()` in
`shared_metadata_gifts.inc:21-33`) over raw obfuscated names; never inline an
address (`audit` fails). Then `tools/check.py`, review golden hashes, hand-update
the adapter list above from the `compare` report, and update docs/site plus
`PGRAZOR_INPUT_URL` and the hosted input asset when the game version changes.

**Not update-proof today:** raw name/offset literals in `.inc` adapters, golden
hashes/fixtures needing manual refresh, no automatic merge of bindings into new
profiles, stale `purpose` strings, and signer/library/metadata hashes not enforced
in code.

## 7. Docs / tests / CI checklist

### 7.1 Documentation

- `CHANGELOG.md` — user-facing changes require an `## Unreleased` section (it does
  not currently exist; create it above `## v0.3.4`). Use `### New features` /
  `### Improvements & fixes`, bold feature names, keep bullets concise. See
  `docs/releases/README.md:7`; rules at `CONTRIBUTING.md:13`.
- `README.md:5` headline blurb and `README.md:9-15` feature/guide links. The
  contributor rule saying "update the feature table" refers to the website, not a
  literal table in README (`CONTRIBUTING.md:14,39`).
- Website: `website/src/featureGuides.js` (guide catalog; stable `id`, fields
  `title, category, summary, where, steps[], notes[]`) and `website/src/content.js`
  `settingGroups` (homepage setting grid). Components derive from these; no edits
  needed in `GuideIndex`, `GuideDetail`, `Features`, `guideData`.
- Website screenshot catalog: `website/src/guideScreenshots.js`; every key must be a
  real guide id (`website/tests/guides.spec.js:5-7`), dimensions must match the
  WebP, real captures only, no private data (`website/README.md:45-62`).
- `docs/ARCHITECTURE.md:27-45` source map and `:73-81` Java ownership: update when
  adding a module or moving ownership. `:83-98` verification map: document the
  Python/build runner for any new Java test (`CONTRIBUTING.md:44`).
- Release notes are added at release time, not in a feature PR:
  `docs/releases/TEMPLATE.md` → `docs/releases/vTAG.md`; rules in
  `docs/releases/README.md`; `footer.md` is appended automatically.
  `release.yml:81-82` rejects a tag whose notes file is missing. No CI check
  rejects a missing changelog entry or website guide — human review enforces it.

### 7.2 Test naming and discovery

- Python: `pgrazor/test/test_*.py`, discovered by `unittest discover` in
  `tools/check.py:24`.
- Node: `pgrazor/test/*-test.cjs`, run individually by `tools/check.py:25-27`.
- Java: no standalone tests. Either wire explicit `javac`/`java` into
  `pgrazor/build.py:87-102` (plus `:113-114` for saved places), or drive a
  `*Test.java` from a Python runner `test_<area>_java.py` (e.g.
  `test_control_java.py`, `test_settings_writer_java.py`, `test_live_feed_java.py`,
  `test_gpx_java.py`, `test_shiny_scan_preferences_java.py`). Document new mappings
  in the architecture verification map.
- Website: Playwright `website/tests/*.spec.js`; `guides.spec.js` enforces unique
  guide ids, canonical ids, >1 step, non-empty `where`, direct-open rendering and
  gallery keys.
- `tools/check.py` order: game binding generate `--check` → audit → routes/gifts
  schema checks → pvp data → `build.py --test-only` → Python unittest → Node tests
  → `node --check` on runtime scripts and composed research bundles.

### 7.3 CI and release

- `.github/workflows/ci.yml` (PRs): Python 3.12, JDK 17, Node 22, SDK
  android-36/build-tools 36.0.0/sources; prepares DEX deps, runs `tools/check.py`,
  `pgrazor/build.py`, `rooted_installer.py --check`, `source_bundle.py`; uploads a
  source/tools artifact only — no APK and no secrets.
- `.github/workflows/release.yml` (manual): validates tag regex and
  `docs/releases/<TAG>.md`, installs both NDKs and pinned CMake/Ninja, prepares
  native deps, runs checks, fetches private game input + signing key, builds
  non-root and rooted products, verifies all six release files with
  `tools/verify_release.py`, uploads `PGRazor-android`, optionally publishes a
  GitHub release with notes+footer, then calls the Discord announcement workflow.
- `.github/workflows/pages.yml` deploys the website on `website/**` pushes.
- `tools/source_bundle.py` blocks APK/keys/logs and ignored dirs, and fails on
  new untracked project source unless staged or `--include-working-tree` is used.

### 7.4 Version pins (do not bump in a feature PR)

- App version: `pgrazor/VERSION` is only the local default (`0.1.8`, stale vs the
  changelog's v0.3.4); releases override it via `PGRAZOR_VERSION` from the tag.
  `build.py:19-23` reads it and writes `BuildVersion.java`.
- Game version: `pgrazor/native/schema/game/current.json` selects
  `0.437.0.json` (version code 2026110902, pinned input SHA). Generated headers and
  derived `routes.json`/`gifts.json` must never be hand-edited.
- `release_apk.py` writes `build-info.json` with the game version read from the APK
  manifest; `device_tested` stays false locally.

## 8. Decision shortcut for "add a new option"

- **UI-only preference** (never read by gameplay): follow 6.3 only.
- **Preference consumed by gameplay**: follow 6.3 + 6.5 (wire frame changes must be
  coordinated between Java and native), then 7 for tests/docs.
- **Behavior requiring a new game hook or obfuscated lookup**: add a stable-kinded
  binding to every version profile and reference its macro (6.6); expect to
  hand-update `.inc` adapters when the game version changes.
- **New native host**: 6.5 step 4 plus registry/API/CMake wiring; verify on device.
- **Anything user-visible**: create the CHANGELOG `Unreleased` section, website
  guide + homepage copy, and tests before calling it done (7.1–7.2).
