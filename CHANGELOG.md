# Changelog

## Unreleased

- Recheck nearby gyms promptly after temporary no-action results instead of excluding them for 60 seconds; keep reserve and retry status visible.

- Add a draggable gym-and-berry overlay that starts/stops the entire Gym Slotter & Berry Feeder while preserving deployment and feeding choices. Active cooldown stops both actions; restart manually after it ends. The master starts off each session. A small status line follows the icon and shows deployment, feeding, and cooldown activity.

- Add Batch evolution: preview a named tag, review candy/item costs and skipped entries, then evolve eligible Pokemon sequentially without opening the cutscene. Include Stop, inventory-confirmed progress, server-reported XP, and optional removal of the selected tag after successful evolution. Preserve other tags and skipped entries; special or ambiguous evolutions are skipped.
- Validate tag-removal request construction during preview and handle replacement Pokemon IDs when confirming evolutions. Evolution and optional tag removal have been user-tested on MuMu.

- Add Walk and Teleport actions to world-map Stop, Gym, and Raid details, including the destination cooldown confirmation.

- Fix Quick Spin losing pending spins during short frame stalls or concurrent settings updates. Continue to revoke automation when settings are disabled or stale.

- Fix buddy care pausing on gift results and carrying a paused state into a newly selected buddy. Wait until the buddy is on the map before opening gifts.
- Add a separate EP overlay visibility setting and compact Clear/Reset controls in the buddy berry-priority picker.

- Consolidate buddy care into the Buddy Emotion Engine dropdown, with ordered berry selection and reserve-aware fallback. Preserve existing berry choices and retire the separate Auto feed buddy control.
- Rename Smart Background Catcher to VGP+ and group its controls under Virtual Go Plus. Gray out dependent settings when their parent is off, while preserving saved choices.

- Keep Buddy Emotion automation independent of location/joystick activation, and show when feeding is blocked by berry reserves, fullness/cooldown, or unavailable game services.

- Add a native Buddy Emotion Engine with automatic snapshots, petting, hunger-based normal-berry feeding, optional gift collection, berry reserves, and a draggable status display using game-reported EP. Preserve recent action history across restarts. Maintenance countdown is an estimate; walking and battles remain manual.

- Fix gym deployment and feeding blocked by an unreliable local closed flag; honor server results and feeding cooldowns, keep placement lockouts separate, and report skipped-gym reasons.

- Fix event collection skipping visible, manually collectable boxes because of an extra local expiry cutoff.

- Add a Gym Slotter & Berry Feeder with separate deployment and feeding switches, Gym-tag priority, optional low-CP fallback, berry reserves, and native request pacing. Deployment and consecutive feeding with Stardust rewards verified on MuMu.

- Add Smart Background Catcher with configurable balls, berries, throw quality, and species priorities. Continue catching while in-game menus are open and consider loaded spawns beyond the standard VGP distance limit, subject to server acceptance.
- Fix Smart Catcher rejecting valid encounter responses with a native VGP error; preserve catch retries and automatic transfer behavior.

- Restore wild-catch cooldown history by preserving native encounter types. Show zero cooldown when no actions are recorded.

- Show the cooldown warning before every user teleport, including coordinates, Favorites, World Map destinations, feeds, hotspots, and in-game map-tap teleport controls.

- Add 14 curated hotspots to the World Map and Quick Menu. Map choices select a waypoint; Quick Menu offers Walk or Teleport, and hotspot teleports show a destination cooldown estimate before confirmation.

- Show a live blue current-location dot on the World Map independently of the red destination pin.

- Add World Map place search for businesses, parks, landmarks, and addresses, biased toward the viewed map area. Selecting a result centers its destination pin; Walk and Teleport remain explicit actions. Search uses Photon/OpenStreetMap and results may be incomplete.

### Using tags for gym defenders and evolution

These controls are in the current development build; they are not part of the published v0.1.7-experimental.2 APK.

**Gym defenders**

1. In Pokémon GO's Pokémon storage, open **Tags**, create a tag named `Gym`, and apply it to Pokémon you want to deploy. `#Gym` is also recognized, but `Gym` is the simplest name to use.
2. Open **PGRazor Settings → Raids, gyms & Dynamax → Gym Slotter & Berry Feeder**, then enable **Auto-deploy to open Gym slots**.
3. Start the feature with its gym-and-berry overlay icon or **Run Gym Slotter & Berry Feeder**, then stay on the game map near a friendly gym within 40 m with an open slot. VGP does not need to be running. The slotter prefers eligible tagged defenders; favorites, your current buddy, injured or deployed Pokémon, and other ineligible defenders are skipped. It also avoids species already defending that gym.
4. Leave **Allow lowest-CP fallback** off to restrict deployment to your tagged choices. Enable it only if you also want eligible untagged Pokémon considered when none of your tagged defenders can be used. Deployment does not remove the `Gym` tag.

Deployment and berry feeding have separate selection switches; the overlay starts/stops the whole feature without clearing either choice. Feeding does not require tagging the defenders. Enable **Show gym automation overlay** for the draggable gym-and-berry icon. Active cooldown stops both actions; restart manually after it ends. It uses Nanab, then Razz, only above your configured reserves, and respects server refusals and feeding limits.

**Batch evolution**

1. Create a tag such as `Evolve` in Pokémon storage and apply it only to Pokémon you want evolved. You can use another tag name; enter that name in **PGRazor Settings → Batch evolution**.
2. Optionally check **Remove this tag after each confirmed evolution** before previewing. It defaults off. When enabled, it removes only the selected tag from successful evolutions; other tags and skipped or failed entries are preserved.
3. Select **Preview batch** and review the eligible count, candy/item costs, and skip reasons. Favorites, your buddy, deployed Pokémon, eggs, Shadow/purified Pokémon, and conditional or ambiguous evolution branches are skipped.
4. Activate a Lucky Egg yourself if wanted, then choose **Start batch** and confirm. Each selected Pokémon evolves once per batch, even if another evolution is available afterward. **Stop** prevents further requests; an in-flight action may still finish.

If an error appears, check Pokémon storage and the completed count before retrying: the evolution may have succeeded even if tag cleanup failed. The engine does not automatically retry uncertain evolutions.

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
