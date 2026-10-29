# Building PGRazor

Local builds read the Settings header version from `pgrazor/VERSION`. Release builds override it with `PGRAZOR_VERSION` from the workflow release tag. This is the PGRazor version, independent of the base game version.

## Requirements

The APK packaging toolchain currently supports Windows. Install Python 3.12+, Node.js 22+, JDK 17+, and the Android SDK. The Python build scripts use the standard library; native compilation also requires Git, CMake 4.4.4 and Ninja 1.13.2.

Default SDK packages are `platforms;android-36.1` and `build-tools;36.1.0`. CI uses API 36 / Build Tools 36.0.0. The gameplay core requires NDK `27.3.13750724`; the ART bridge requires NDK `29.0.14206865`. Install both: they serve different compiler requirements.

## Fresh checkout setup

From the repository root, with Python 3.12 selected as `python` and `sdkmanager` on PATH:

```powershell
$env:ANDROID_SDK_ROOT = 'C:\Android\Sdk'
$env:ANDROID_HOME = $env:ANDROID_SDK_ROOT
$env:PGRAZOR_ANDROID_PLATFORM = 'android-36'
$env:PGRAZOR_BUILD_TOOLS = '36.0.0'
python -m pip install "cmake==4.4.4" "ninja==1.13.2"
sdkmanager "platforms;android-36" "build-tools;36.0.0" "ndk;27.3.13750724" "ndk;29.0.14206865"
$env:ANDROID_NDK_HOME = Join-Path $env:ANDROID_SDK_ROOT 'ndk/27.3.13750724'
$env:PGRAZOR_ART_NDK = Join-Path $env:ANDROID_SDK_ROOT 'ndk/29.0.14206865'
$env:PGRAZOR_NATIVE_DEPS = Join-Path (Get-Location) 'pgrazor/build/native/deps'
python -B tools/prepare_native.py --download
```

Run each command successfully before continuing. On Windows with several Python versions, use `py -3.12` in place of `python` consistently. The explicit `--download` permits fetching the pinned LSPlant, DexBuilder, parallel_hashmap, xz-embedded and SHA-verified Dobby sources. Only the required Git submodules are initialized. Existing modified or incorrectly pinned dependency trees are rejected; setup never resets them.

Keep these environment variables for subsequent builds. `PGRAZOR_NATIVE_DEPS` selects the dependency cache; `--cache PATH` is also accepted by setup and `art_build.py`. Without an override, a fresh checkout uses `pgrazor/build/native/deps`; existing development checkouts can reuse their verified `.artifacts/native-migration` cache. ART NDK discovery also supports `ANDROID_SDK_ROOT`/`ANDROID_HOME` and the existing local fallback, while gameplay builds use `ANDROID_NDK_HOME`.

For subsequent offline prerequisite verification, without fetching or compiling:

```powershell
python -B tools/prepare_native.py
python -B pgrazor/art_build.py --check
python -B tools/check.py
```

The first two commands verify dependency commits/gitlinks, source cleanliness, pinned NDK revisions and build tools. `tools/check.py` runs the repository checks. ART builds retain their CMake objects, static archives, link maps, unstripped libraries and provenance receipts under `pgrazor/build/art`; preserve those outputs and the recorded dependency sources for relinking.

## Local playable build

Supply your own unmodified, Niantic-signed ARM64 game APK/APKM. The supported input is Pokémon GO 0.433.1, version code 2026101502. The release workflow additionally pins its SHA-256:

```text
aabe3f866bb20b008d10646d35615fe8845ad0164ddc5ac7984e1a5565836e53
```

Keep the original under the ignored `pgrazor/input/` directory. From the repository root:

```powershell
python -B pgrazor/nonroot.py --apk pgrazor/input/game.apkm
python -B pgrazor/release_apk.py --run pgrazor/build/nonroot/RUN-ID --output pgrazor/build/release
```

Use the run directory printed by the first command. The second command merges the library-only ARM64 configuration split, removes split-install requirements, labels the installed application Pokémon GO (the in-game menu remains PGRazor), aligns and signs **PGRazor.apk**, and writes checksums. It rejects resource/code splits rather than silently dropping them. No command installs anything on a phone.

Single-APK packaging is new and still needs a disconnected-phone install/launch check. Existing split builds remain available in the run directory as a rollback option. Preserve a tested installation until the new candidate has been checked.

## Runtime selection on phones and MuMu

The default game build selects compiled production gameplay, including the standard feature hosts. It packages compiled core and ART libraries, Java controls/bridges and rewritten DEX calls. Production asset audits reject Frida and PGRazor JavaScript. The game libraries remain ARM64; MuMu translates those libraries while its Android Java process runs as x86-64.

Compiled Java/ART bridges own location and lifecycle integration. Typed native transport52 exchanges settings and events, including between host and translated guest namespaces on MuMu. The verified render boundary drives native Unity scheduling and host orchestration. Stale snapshots revoke feature leases; no root daemon or external controller is required.

Translated executable code is checked against ARM ELF executable segments and instruction fingerprints. Hook placement, managed-root lifetimes and ART publication have dedicated native fixtures. Current compiled-only live checks are on research MuMu; earlier phone checks of legacy runtimes do not validate this cutover on physical Android. Consult the latest migration receipt for exact gameplay coverage.

Frida is available only through explicit `--research-runtime` selection for supported survey, scene, interaction and camera probes. Retained research scripts, the pinned bridge and Gadget provisioning support those builds; they are excluded from normal production APKs. Check `nonroot.py --help` for compatible research flags. The release build information records bundled native architectures separately from the game's ARM64 architecture.

## Source/tools archives

`python -B tools/source_bundle.py` archives tracked working-tree source, omitting tracked files deleted from the working tree. It fails clearly if allowlisted new project source would otherwise be omitted. Review those files before staging them or explicitly use `python -B tools/source_bundle.py --include-working-tree` for a local archive.

The explicit option adds only nonignored source beneath `pgrazor/native`, `pgrazor/art`, `pgrazor/src`, `pgrazor/test`, `tools/dex_bridge`, and named native/ART/schema build modules. It does not collect arbitrary untracked notes or handoffs. Both modes reject selected credential/generated paths and symlinks/junctions, including linked parent directories. The option does not stage or commit files; review the archive before distributing it. Inputs, signing keys, device evidence and retention history remain local.

## Signing

Local development creates an ignored `build/pgrazor-development.jks` key with alias `pgrazor`. This key uses a development password; configure a dedicated strong-password keystore for releases. Pre-release builds do not migrate previous pre-release settings or identifiers.

For explicit signing, set `PGRAZOR_KEYSTORE`, `PGRAZOR_KEY_ALIAS`, `PGRAZOR_STORE_PASSWORD`, and `PGRAZOR_KEY_PASSWORD`. Passwords are passed to Android signing tools through environment variable references, not command-line values. A missing configured key or password fails the build. Keep the same certificate across updates.

The `build.py` output named `pgrazor-preview.apk` is a controls preview, **not the game**. Do not publish it as a working PGRazor release.

## GitHub Actions: APK releases

Configure these Actions secrets:

| Secret | Value |
| --- | --- |
| `PGRAZOR_INPUT_URL` | HTTPS URL to the supported original APKM, or a GitHub release-asset API URL in this repository |
| `PGRAZOR_KEYSTORE_BASE64` | Base64-encoded release keystore |
| `PGRAZOR_KEY_ALIAS` | Signing alias, e.g. `pgrazor` for PGRazor |
| `PGRAZOR_STORE_PASSWORD` | Keystore password |
| `PGRAZOR_KEY_PASSWORD` | Private-key password |

For a private GitHub input asset, use its `https://api.github.com/repos/OWNER/REPO/releases/assets/ID` URL. The workflow supplies its scoped token; authorization is removed when GitHub redirects to the signed download host.

Run **Actions → Playable APK release → Run workflow**, enter a version tag such as `v0.1.0`, and leave `publish` off for a candidate. Download **PGRazor-apk** from the completed run and extract **PGRazor.apk**. Repository read access is required to download private [workflow artifacts](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/download-workflow-artifacts).

After device testing, run with `publish` enabled to create a full release containing the APK, checksums, and build information. An existing tag/release is not overwritten. The workflow installs the pinned NDKs/build tools and prepares native dependencies before checks and packaging. Its PowerShell setup writes paths to `GITHUB_ENV` so later steps inherit the same compilers and cache. The workflow is manually triggered, does not run on pull requests, and requires a versioned release-notes file at `docs/releases/TAG.md`. The shared `docs/releases/footer.md` is appended automatically, including the optional Ko-fi support link. No secrets or game files are uploaded by source CI.

The published release contains the installable APK, checksum, build information, and the versioned changelog. The signing key remains an Actions secret; keep it unchanged across releases.

## Discord release announcements

Create an incoming webhook in Discord channel `1562582290536726598` under **Edit Channel → Integrations → Webhooks**. Save its URL as the repository Actions secret **DISCORD_RELEASE_WEBHOOK**. Do not commit the URL.

The **Discord release announcement** workflow posts published releases (including labeled prereleases) with release notes, a GitHub link, an APK link when present, and the optional Ko-fi support link. It verifies the webhook's channel before sending. Drafts are not announced. The APK release workflow calls it directly because releases created with `GITHUB_TOKEN` do not trigger another release-event workflow.

To ping a release subscriber role, set the repository Actions **variable** `DISCORD_RELEASE_ROLE_ID` to its numeric role ID (Discord Developer Mode → right-click the role → Copy Role ID). Ensure Discord allows the webhook to mention that role. Only this role is allowed to ping; other roles, users, and everyone mentions remain disabled. Leave the variable unset to send without a ping. This also applies to prereleases and manual announcements.

Without the secret, the announcement step logs a warning and sends nothing. To announce an existing release after setup, manually run **Discord release announcement** with its tag. Manual runs and reruns can post another announcement, so check the channel before retrying. This posts a channel message; it does not crosspost to channel followers.

## Installation and updates

Install the single APK with Android's package installer, or use `adb install -r PGRazor.apk` from a computer. Never uninstall a working PGRazor build just to bypass a signing error; use a build signed with its existing key. An official-store game cannot be replaced in place by a differently signed build. Local application data may be lost if you choose to uninstall it.

The minimum SDK is Android 9 (API 28), but native gameplay hooks are supported only on the pinned ARM64 game build. No emulator, future-version, or all-device compatibility is implied.

## Retention

`cleanup.ps1` removes recognized obsolete generated APK files after packaging. It preserves diagnostic records, keys, inputs, and runtime dependencies. Copy `build-retention.example.json` to the ignored `build-retention.json` on a new checkout; packaging also initializes it if missing and pins the first completed candidate (not a tested rollback). Pin deployed builds and a tested rollback before replacing them. Never commit device-specific retention history.
