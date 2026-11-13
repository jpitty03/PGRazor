# PGRazor Root installer (`dev.pgrazor.root`)

Standalone Android installer APK that carries the PGRazor Root Zygisk module in its assets and
installs it with the phone's root manager. It is the rooted counterpart of the patched `PGRazor.apk`;
users who are not rooted keep using the patched APK.

## What it does

- Shows device status honestly: root access, root manager (Magisk 27+, KernelSU, APatch), Zygisk
  provider, the installed PGRazor module, and the official Pokémon GO install (version + signature).
- Installs the bundled module on an explicit **Install** tap, through the detected manager:
  - Magisk: `magisk --install-module <zip>`
  - KernelSU: `<ksud|/system/xbin/dusk|/data/adb/ksud> module install <zip>`
  - APatch: `<apd> module install <zip>`
- Surfaces the full command output in an on-screen log and reports clear errors.
- Offers **PGRazor: On / Off** and an always-available **Reboot** button. Pending module changes show **Reboot to activate**, including after reopening the installer. Nothing reboots automatically.

All root work runs off the main thread with a bounded process wait; a timed-out root-manager command may continue in the background; detection is
read-only.

## PGRazor: On / Off

A module built with the launch-time switch carries an empty `pgrazor/toggle-v1` capability
marker; the installer uses that marker to tell a toggle-aware module from an older one. Once
such a module is installed, PGRazor can be turned **On** or **Off** without reinstalling or
disabling the module in the root manager:

- **Off** is a *soft* switch: the installer creates an empty `pgrazor/injection-off` flag in
  the live module. At the game's next cold start the Zygisk loader sees the flag and declines to
  inject, while the module itself stays enabled. **On** removes the flag so the next launch injects
  again.
- Because the flag is read once per process start, flipping it only takes effect on the game's next
  launch. The installer therefore **force-stops the game** when you change the switch; **no reboot**
  is required to turn PGRazor On or Off. (A one-time reboot is still needed the first time you
  install or update the loader itself, as with any Zygisk module.)
- **Reboot** is always offered when root access is available — first install, a loader
  update, or enabling Zygisk.
- While a freshly installed or updated module is staged but not yet active, the status shows a
  pending-activation label until you reboot once.

The module never ships in the Off state — a fresh install defaults to On. When you **update** a
module that is currently Off, the module's `customize.sh` carries your Off choice forward from the
live module into the update; if it cannot preserve that choice it **aborts the install** rather than
silently turning PGRazor back On. This holds both for updates installed straight through the root
manager and for updates installed by this APK. Staging directories are wiped and re-extracted by the
manager before `customize.sh` runs, so the live module is the single source of truth for the switch.

## Files

- `AndroidManifest.xml` — package `dev.pgrazor.root`, launcher activity and a game-only restart helper, `<queries>` for the
  game package so its version/signature are visible on Android 11+. `versionCode`/`versionName` are
  overwritten per build.
- `src/dev/pgrazor/root/InstallerActivity.java` — the UI (programmatic, no resources).
- `src/dev/pgrazor/root/RootStatus.java` — detection, honest status, install and reboot.
- `src/dev/pgrazor/root/RestartActivity.java` ? foreground account-switch restart, with an OS-verified calling-package check and no root commands.
- `src/dev/pgrazor/root/RootShell.java` — bounded `su -c` execution.
- `InstallerConfig.java` is **generated per build** by `rooted_installer.build_installer` (game
  target, PGRazor version, module asset name, and the trusted Niantic certificate digest reused from
  `nonroot.NIANTIC_CERT_SHA256`). It is not checked in, the same way `build.py` generates
  `BuildVersion.java`.

## Build

`../rooted_installer.py` exposes:

```python
build_installer(module_zip, target, version, folder) -> Path   # signed installer APK
```

It uses the project's offline SDK/JDK tooling (`build.TOOLS`, `build.ANDROID`) and
`build.signing_args()` — the same key as the patched APK.

`rooted.package()` builds this installer after assembling the module and records both hashes in its report. The release workflow includes the installer alongside the patched APK.

For a standalone build from an existing run:

```
py -3.12 -X utf8 pgrazor/rooted_installer.py pgrazor/build/rooted/<run>/report.json
```

## Limitations

- Root remains experimental; native hook installation can still fail on some Samsung devices.
- **Zygisk status is best-effort.** For Magisk it reads `magisk --sqlite`; for KernelSU/APatch it
  looks for the `zygisksu`/`rezygisk` provider module. When neither can be read it reports "Unknown"
  rather than guessing. Zygisk being off is not blocked at install time — the module installs, but
  PGRazor will not load until Zygisk is active.
- **Install is gated only on root + a supported manager.** A mismatched game version, an untrusted
  game signature, or a missing game are reported as warnings, not blockers: the version/signature
  gate is enforced at runtime by `RootedRuntime`, and the module can be installed before the game.
- Installer build and install/reboot have been exercised on MuMu instance 1 with KernelSU and Zygisk Next. Magisk/APatch device installation remains unverified. See the internal receipt for candidate hashes and gameplay evidence.
- On / Off, old-module gating, pending reboot after reopening the installer, and Off surviving an update/reboot were exercised on rooted MuMu instance 2 with KernelSU and Zygisk Next. Magisk/APatch and physical rooted phones remain unverified.
