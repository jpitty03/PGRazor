# PGRazor Root

PGRazor Root is a separate installer (`dev.pgrazor.root`) for the official, Niantic-signed Pokémon GO app. It carries a Zygisk module and installs it through Magisk 27+, KernelSU, or APatch. KernelSU and APatch also need a Zygisk provider. The patched `PGRazor.apk` remains the non-root build.

This is development work. See the internal build/device receipt for the exact validation status before distributing a candidate. Installing the module does not modify the game's APK or its signing certificate.

## Installation

1. Install `PGRazor-Root.apk` and open it.
2. Check status and grant the installer root access in your root manager.
3. Confirm that the official game version matches the version displayed by the installer.
4. Tap Install, review the result, and reboot when prompted.
5. Open Pokémon GO. The PGRazor panel appears when the module starts successfully.

To turn it off, disable the `pgrazor` module in your root manager and reboot. Uninstalling the installer APK alone does not uninstall the module.

For manual installation, install `PGRazor-Root-module.zip` through your root manager's module installer and reboot. Use the official game version supported by that release. You can also install `PGRazor-Root.apk` for status checks and automatic account-switch restarts; you do not need to install the module again through the APK. Without the installer APK, account switches require manually closing and reopening the game. `PGRazor.apk` is the separate non-root patched game, not the Root companion APK.

The runtime requires the exact packaged game version and official signing certificate. A newer game is not automatically accepted just because Java bridge names still match: native gameplay also depends on the supported game build. Root status and Zygisk configuration do not by themselves prove injection succeeded; the in-game Run log records startup.

## Build

Use the SDK, JDK, pinned NDKs, and signing environment described in [BUILDING.md](BUILDING.md). From the repository root:

```powershell
py -3.12 -B pgrazor/rooted.py pgrazor/input/game.apkm --download --output dist/release
```

`--skip-build` reuses the existing DEX only after validating its source fingerprints. `--module-only` produces the module ZIP for development without the installer. The source APK/APKM must have the trusted game certificate; bridge validation runs before packaging.

Each run lives in `pgrazor/build/rooted/<run>/` and records module, payload, DEX, installer hashes, supported game version, and architectures in `report.json`. Release output contains `PGRazor-Root.apk`, `PGRazor-Root-module.zip`, `root-build-info.json`, and checksum entries for both Root downloads in `SHA256SUMS.txt`. The release workflow uploads both APKs and the manual module ZIP as build artifacts and, when publication is requested, as release assets. Publication remains a separate workflow input.

`cleanup.ps1 -RootedOnly` previews rooted cleanup; add `-Apply` to execute it. Packaging invokes this cleanup after completing its report. Copied `input-*.apk` files are expendable once a complete module has been verified. The latest three run folders and `pinnedRootedRuns` entries in local `build-retention.json` protect module/installer outputs. Keep the deployed candidate and a tested rollback pinned. Cleanup preserves reports, original inputs, source, and keys, and writes a receipt under `build/cleanup/`.

## Startup and native binding

The Zygisk loader selects only the game's main process, reads the DEX and payload before specialization, then starts the Java runtime as the game UID. The runtime verifies the game target, installs the Java bridges, and calls the shared PGRazor startup. A partial bridge installation remains inactive and forwards original calls.

On MuMu, ART hooks use the process's real x86_64 ABI, determined from `/proc/self/exe`. Gameplay uses the translated ARM64 core. Rooted native loading waits for Unity's render message so the game can establish its translated library namespace first.

The native bootstrap first retains its existing `dlopen(..., RTLD_NOLOAD)` lookup. If Android namespace separation prevents that lookup, it discovers an exact, unique loaded image and uses Android's loader with that image's namespace. It retains `RTLD_NOLOAD`, never force-loads another IL2CPP runtime, and leaves unsupported loaders unavailable. The mechanism follows the caller-address namespace selection in [Android's dynamic linker](https://android.googlesource.com/platform/bionic/+/master/linker/linker.cpp) and [libdl](https://android.googlesource.com/platform/bionic/+/master/libdl/libdl.cpp).

Native startup is confirmed by `native-bootstrap-ready` followed by `native-scheduler-ready`. These markers establish binding and scheduler startup; feature behavior still requires gameplay checks. Shared changes also require a patched APK device regression check before a public release.

The standalone native checks can be reproduced against a reviewed core:

```powershell
py -3.12 -B pgrazor/native_harness.py runtime_bootstrap runtime_bootstrap_namespace --serial DEVICE_SERIAL --adb PATH_TO_ADB
```

These harnesses do not operate the game. Their receipts record the actual library hash and test output.

## Rooted UI

Run log opens inside the game and can save a copy to the game's external files folder. Settings backup also uses that folder, at `PGRazor-settings.json`; copy the file elsewhere before uninstalling the game. Import reads the same path and asks before replacing settings.

Account switches are queued durably and applied before Unity starts in the next process. The installer provides a foreground restart helper so it can reopen the game after the old process exits. Module-only installs without the installer require manually closing and reopening the game. The helper accepts only requests whose calling package is the official game and performs no root or account-data operations. The game's official APK does not need PGRazor activities added to its manifest.

The pinned Zygisk API is v5, whose [upstream compatibility table](https://github.com/topjohnwu/zygisk-module-sample#api) requires Magisk 27 or newer. Older Magisk versions are rejected by the installer. KernelSU/APatch require a provider supporting API v5.
