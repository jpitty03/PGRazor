# Changelog

## Unreleased

## v0.1.7-experimental.2

[Full release notes](docs/releases/v0.1.7-experimental.2.md)

- Fix Rocket, Gym, Raid, and Dynamax battle automation observing natural game callbacks.
- Resume VGP after confirmed inventory cleanup; query selected item counts and process large cleanup batches incrementally.
- Allow other boxes to collect after a missing encounter-animation callback while preserving late-screen suppression.

- Keep nearby autowalk's default at 50 stops while allowing counts up to all currently loaded unique PokéStops and Gyms. The displayed maximum follows nearby availability; leaving the count blank selects All.

## v0.1.7-experimental.1

[Full release notes](docs/releases/v0.1.7-experimental.1.md)

- Add an Events screen and a Halloween toggle to open visible boxes and skip encounter screens.
- Rewrite the gameplay runtime in compiled native C++ and Java.
- Fix buddy berry feeding on MuMu and Routes pausing during location updates.
- Stop periodic full inventory scans when auto-discard is disabled. Item settings request counts while visible, and berry controls query only berry stock; unchanged catalogs are no longer republished.
- Fix the native VGP pilot pausing after an unsent request is refused during a routine settings refresh. Restrictive settings still stop requests; uncertain outcomes remain paused.

## v0.1.6

[Full release notes](docs/releases/v0.1.6.md)

- Add Switch account to the Quick Menu and deletion of inactive saved accounts, including their remembered sessions and local profile data.

- Add experimental account profiles under Settings for Google/PTC sign-in, with separate settings, last location, cooldown history, favorites and saved routes. Switching stops automation and restores an encrypted saved session after restarting; each account needs an initial sign-in, with normal sign-in fallback when its session expires.

- Show the PGRazor version beside the theme button in Settings.

- Automatically dismiss Adventure Sync prompts when location spoofing is enabled and Android location permission is denied.

- Add experimental in-game Route walking from the quick menu: discover nearby Routes, choose the shortest total walk, follow the recorded path, and confirm rewards before continuing, with a configurable completion limit and a draggable icon for live path details.

## v0.1.5

[Full release notes](docs/releases/v0.1.5.md)

- Add an Always hit throw option that converts missed ball collisions into hits while preserving normal catch outcomes.
- Preserve the LivePokeMap login form during password-manager autofill and support username/password autofill hints.
- Use one latitude, longitude field for quick-menu teleport, accepting coordinates with or without a space after the comma.

## v0.1.4

[Full release notes](docs/releases/v0.1.4.md)

- Ship one APK for ARM64 Android phones and MuMu, selecting the appropriate runtimes automatically.
- Connect MuMu's Android controls to translated ARM gameplay hooks for Quick Catch, VGP, berries, transfers, IV display, and other native features.
- Fix early permission initialization so spoofing can start with app location permission denied.
- Adapt gift dialog observation and executable-code validation for MuMu while retaining the existing phone runtime path.

## v0.1.3

[Full release notes](docs/releases/v0.1.3.md)

- Ask before a game-map tap stops a planned route or Autowalk; cancelling keeps the route running.

- Avoid replacing the native location-update method; normalize provider events through listener adapters to address Android runtime dispatch crashes.

- Add My location centering and a direct Clear route action to the world map.

- Import GPX waypoints, routes, and tracks; save and export routes from the world map and choose saved GPX routes in Autowalk.

- Recover Dynamax attack controls from the active battle when their setup callbacks were missed, allowing automatic quick and charged attacks to start.
- Vary the circle radius within the selected Nice, Great, or Excellent throw band, preserving one sampled value across a throw's submit/hit callbacks.
- Separate native feature state machines, GPS delivery, VGP/toast policy, and map tile/POI layers into modules with explicit ownership.
- Split the settings menu into named section builders and refresh binders, share theme styling, and stop item-dialog polling when its panel closes.
- Save settings through an ordered background writer, with flush checks for export/import and protection against stale reloads after a failed save.
- Discover all device-free test suites in CI, replace moved-feature source-slice fixtures with module tests, and record hashes for composed runtime scripts and their sources.
- Keep map tile requests tracked until their completion callback runs, avoiding a queue-overflow race during viewport changes.
- Restore catch animation speed and release IV labels during cleanup even when the runtime clock fails.

## v0.1.2

[Full release notes](docs/releases/v0.1.2.md)

- Allow location spoofing with Android's system Location switch off or the app's location permission denied. System Location can stay enabled for other apps; disabling spoofing restores normal device-location requirements.

- Keep spoofed location accuracy, timestamps, and provider status consistent when real GPS updates arrive.

- Make the joystick more transparent with no backing panel, retaining the drag handle; add a tap-to-walk/double-tap-to-teleport game-map mode.

- Add saved 100%, 75%, 50%, and 25% map-overlay UI scale presets under Location & Overlays.

- Simplify the catching berry overlay to an icon/count and add three saved berry priorities with inventory fallback, including out-of-stock selections.

- Add a searchable custom species keep list and optional persistent Keep All transfer overlay, shared by manual and VGP catches and included in settings backup.

- Allow VGP to run while other in-game screens are open; exclude the current manual encounter target and keep world markers map-only.

## v0.1.1

[Full release notes](docs/releases/v0.1.1.md)

- Restore the saved Location spoofing toggle; disabling returns to device location and stops simulated movement.

- Add separate saved IV/level display toggles for encounters and owned Pokémon details, included in settings export/import.

- Quick Catch can skip creating the post-catch details screen for confirmed manual catches, preserving auto-transfer and the game's dismissal callback. Unsupported paths retain normal dismissal.

- Quick Catch also shortens the successful-catch presentation wait to 100 ms starting with the first catch. Summary dismissal and auto-transfer retain their existing completion checks.

## v0.1.0

[Full release notes](docs/releases/v0.1.0.md)

- Fix Google sign-in by opening Google authentication in the system browser.
- Add optional Discord release role pings.

- Pause automated gym, raid, and Dynamax attacks and dodges while the Pokémon switch picker is open; resume after it closes.

## v0.1.0-alpha.4

[Full release notes](docs/releases/v0.1.0-alpha.4.md)

My Shinies, expanded Feed display filters and local presets, plus portable settings export/import.

## v0.1.0-alpha.3

[Full release notes](docs/releases/v0.1.0-alpha.3.md)

Quick menu, map layers and routes, LivePokeMap integration, movable overlays, unlimited egg incubation, VGP improvements, Discord footer, and restored Pokémon GO launcher label.

## Earlier pre-release work

- Rename the project, Java namespace, assets, settings, logs, signing alias, and tooling to PGRazor throughout; remove legacy settings migrations. A fresh installation is required.

- Added independent XXS and XXL keep rules for caught-Pokémon transfers, shared by manual catches and VGP.
- Changed the VGP settings switch to control overlay visibility; the overlay power button starts/stops VGP and displays ON/OFF.
- Added a compact VGP activity display, projected target marker, caught-ID transfer integration, and item cleanup sequencing.
- Introduced private single-APK packaging and a guarded GitHub Actions prerelease workflow.
- Prepared source-only CI artifacts, build/contribution documentation, licensing notices, and repository exclusions.

The new single-APK release path and latest UI changes still require device testing.

Earlier release notes are available on [GitHub Releases](https://github.com/jpitty03/PGRazor/releases).
