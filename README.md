<p align="center"><img src="pgrazor/branding/pgrazor.svg" width="88" alt="PGRazor logo"></p>
<h1 align="center">PGRazor</h1>
<p align="center">A compact Android interface for location controls and Pokémon GO automation.</p>

PGRazor adds an in-game menu, independent joystick, encounter controls, and optional automation. Settings persist across restarts, with light and dark themes.

Explore [the PGRazor website](https://jpitty03.github.io/PGRazor/) for screenshots, a settings guide, and installation steps.

Download the installable **PGRazor.apk** from [Releases](https://github.com/jpitty03/PGRazor/releases). Each release includes a changelog, SHA-256 checksum, and build information. Signing keys, original build inputs, and investigation logs are excluded from Git. Selected map artwork includes [third-party notices](THIRD_PARTY.md).

## Install a release

1. Open [Releases](https://github.com/jpitty03/PGRazor/releases).
2. Download **PGRazor.apk**, not a source archive or preview.
3. Transfer it to your ARM64 Android phone, allow installation from your chosen file manager, and open the APK.
4. Launch **Pokémon GO** (the installed app name) and tap the small **R** icon to open the quick menu. Choose **Settings** for the full-screen controls.

Pre-releases are test candidates; check their release notes for device-validation status. The current game baseline is **Pokémon GO 0.433.1 / 2026101502**. Native hooks are version-specific. A different game version can leave features unavailable.

The Android package identity stays compatible with the game. An official-store installation uses a different signing certificate and cannot be updated in place with a PGRazor build. See [installation and builds](docs/BUILDING.md).

## Main menu

<p align="center"><img src="website/public/images/main-menu.png" width="320" alt="PGRazor dark-theme main settings menu with feature sections and Discord footer"></p>

## Features

Features listed here include unreleased changes on `main`; check your release notes for what is included in your installed APK.

| Area | Controls |
| --- | --- |
| Map | Optional location spoofing, including with app location permission denied; camera-relative joystick, saved locations, GPX import/export and saved routes, Autowalk, tap-to-walk/teleport, nearby stop/gym/raid layers |
| Catching | Quick Catch; Always Hit; Nice/Great/Excellent throws; always curveball; optional AR+ flag; optional level and color-graded IV display; berry selector |
| Inventory | Optional IV/level on Pokémon details; transfer newly caught Pokémon below 90% IV; configurable item limits |
| Keep rules | Legendary/mythical/Ultra Beasts, shiny, event/costume, background, **XXS**, **XXL**, searchable custom species keep list, and a persistent Keep All overlay; favorites and eggs always kept |
| Virtual Go Plus | Nearby wild catches with red balls, nearby stop spins, optional notifications, target marker and activity status |
| Eggs | Unlimited incubator only; shortest- or longest-distance priority |
| LivePokeMap | Remote Pokémon, stops, gyms, raids, and stations; requires a LivePokeMap subscription |
| Battles | Rocket quick/charged attacks, charged minigame and shields; raid/gym/Dynamax attacks, Max attacks and dodging |
| Other automation | Selectable buddy berry, gift opening/sending with optional stickers, quick spin, local spawn booster |
| Interface | Quick menu, full-screen settings, separate joystick, cooldown history, light/dark themes, settings export/import |
| Accounts (experimental) | Quick Menu switching, remembered Google/PTC sessions, deletion of saved accounts, and separate settings, location, cooldown history, favorites and saved GPX routes |

**Accounts:** Open **Quick Menu → Switch account** or **Settings → Accounts** to manage profiles. Delete removes an inactive profile and its saved sign-in, settings, location, favorites and routes from this device; it does not delete the Pokémon GO account. Switch away before deleting the current profile. Switching stops movement and automation, saves the outgoing account, and restarts the game with the selected account's remembered session. Sign in once per account to establish a saved session; expired or revoked sessions may require sign-in again. Sessions are encrypted on-device using Android Keystore, excluded from settings exports and Android backups, and never include saved passwords. The signed-in game identity determines the profile, including when you choose a different account at login. Existing settings become the first profile; new profiles start with defaults. LivePokeMap authentication remains shared, while its filters are per profile. Settings export/import applies to the current profile and does not export the account list. If sign-in choices are missing while signed out, **Accounts → Reset sign-in choices** restarts the game and asks for your date of birth again. Enter your own date of birth; the game controls which providers are available.

**Quick menu:** Map, speed presets (0, 9.3, 16, 30 km/h), Autowalk, In-game Routes, Feeds, Favorites, Teleport, Switch account, Events, and Settings. Favorites shares the map's saved places. Autowalk builds a route through up to 50 nearby loaded stops and gyms, with optional looping. Routes use direct segments between locations. Zero speed pauses travel; selecting another speed resumes the current route. Backgrounding stops routes rather than resuming movement automatically.

**Events:** Open **Quick Menu > Events > Halloween 2026** to enable **Open boxes and skip encounters**. It opens visible boxes and collects rewards, skipping its Pokemon encounter screens without catching. It does not filter for candy-only boxes.

**Transfer protection:** Open **Transfer on catch > Custom Keep List** to keep selected species (including all forms) from manual and VGP catches. Enable the transfer overlay for a quick **Keep All** toggle. Turning Keep All off restores your existing rules; it does not enable auto-transfer if its main setting is off. The list and toggle state are saved across restarts.

**Catching berries:** Tap the berry icon/count overlay to choose up to three berries in priority order. Empty-stock berries can be selected; each throw uses the first stocked choice. After three selections, the next berry tap starts a new list. Off clears the list. Priorities are saved across restarts and included in settings backups.

**Auto incubation:** Uses only the unlimited incubator, with shortest- or longest-distance egg priority. Checks while on the game map and starts another egg after the incubator becomes free. Limited-use, Super, and timed incubators are not used.

**Overlay positions:** Drag the cooldown timer, VGP icon, or Keep All icon to reposition it. Positions are saved; the VGP status label follows its icon. A tap opens cooldown history or toggles the corresponding feature.

**UI scale:** Location & Overlays offers 100%, 75%, 50%, and 25% sizes for the map launcher, quick menu, joystick, cooldown timer, VGP overlays, catching Xfer/Keep All icon, and berry overlay/picker. The choice is saved and included in settings backups.

**Game map taps:** Choose Do nothing, Walk, Teleport, or Walk / double tap teleport. The combined mode walks on a single ground tap and teleports on two quick taps at the same location.

**VGP:** the settings switch shows or hides its overlay. Tap the connected/disconnected device icon to start or stop VGP; ON/OFF labels show its state. VGP starts off and switches off when the app leaves the foreground. Optional **Stop on cooldown (estimated)** also switches it off until you manually restart it. Catch and item automation follow their existing settings and protection rules.

Smart catching can continue through in-game menus such as Pokémon storage using nearby spawns already loaded by the game. Manual wild encounters still pause it; leaving the app still stops VGP.

Smart Catcher attempts loaded, visible wild Pokémon without the normal VGP's local 40 m cutoff. The game still determines encounter eligibility; a declined encounter is skipped before using a berry or ball. This does not guarantee catches at every visible distance.

**VGP+ (local test build):** Open its button in the VGP settings section and enable the mode, then use the normal VGP Start/Stop icon. Choose a standard ball, optional berry, throw rating, and curveball. Optional rules use Ultra Balls and Golden Razz for shiny or IV ≥90%, Ultra Balls for configured CP/level thresholds, and Pinap for selected candy species. Premium rules take priority over candy rules. Empty selected stock falls back to available regular balls or skips the berry; Master Balls are never used. Throws wait 1.2–1.8 seconds, with at most three additional throws after confirmed escapes. Existing storage, transfer, foreground and manual-interaction gates remain in effect. Requested throw ratings do not guarantee a catch.

**Gym Slotter & Berry Feeder (local test build):** Open Settings > Raids, gyms & Dynamax > Gym Slotter & Berry Feeder. Deployment and feeding are independently enabled and default off; VGP need not be running. While the game is on the map, it considers friendly gyms within 40m, checks current defenders, and prioritizes eligible Pokémon tagged `Gym` or `#Gym`. The optional fallback chooses low CP and excludes shiny, lucky, and IV ≥90% Pokémon; all candidates exclude favorites, the current buddy, injured, deployed, and otherwise ineligible Pokémon. Choose deployment, feeding, or both, then use Run Gym Slotter & Berry Feeder or its draggable gym-and-berry icon to start. Show gym automation overlay controls visibility. The master starts off each session. Active cooldown stops the whole feature and blocks a new start; restart manually afterward. Stopping preserves both selections. Feeding uses surplus Nanab then Razz, with separate reserves (default 50), at least 1500ms between confirmed feeds, and bounded per-defender attempts. Server refusals and inventory updates govern subsequent requests; rewards are reported only from successful responses. Ambiguous timeouts pause the feature; switch both options off to reset the pause. Deployment and consecutive feeding with Stardust rewards have been verified on MuMu. Auto-spin Gym discs is a separate, default-off choice under Virtual Go Plus > VGP+: it works at any team's loaded gyms within 40 m, checks bag space, and respects each disc's cooldown. Enable VGP+ and the choice, then start VGP; Gym Slotter and feeding need not be enabled.

**Buddy Emotion Engine (local test build):** Expand Settings ? Buddy Emotion Engine; it replaces Auto feed buddy. The master checkbox controls snapshots, petting, hunger-based berry feeding, and optional gift collection. Select berries and arrange their priority; each feed uses the first selected berry whose stock exceeds the per-berry reserve. New configurations start with Nanab, Razz, then Pinap and a reserve of 50; premium berries require explicit selection. Poffins are never used. Existing single-berry choices are preserved, and legacy-only Auto feed buddy settings migrate with their old no-reserve behavior. Disabling the engine grays its controls without clearing choices and does not reactivate the old feeder. The optional draggable status display shows game-reported EP/excitement and an estimated maintenance countdown. Use Show EP overlay to hide it without stopping buddy care. Recent action history is scoped to the current account and buddy and survives restart. Unknown request outcomes pause automation until toggled off. Snapshot/pet EP gains, a three-berry Nanab feed sequence, and restart history have been verified on MuMu. Ordered fallback has native fixture coverage; live fallback, gifts, and sustained excitement still need verification. The game reported 32 EP and Excited during a live run; gift result handling and buddy-swap pause recovery now have native regression coverage. Walking and battles remain manual; automatic battle forfeits are not implemented.

**LivePokeMap feeds:** Requires an active subscription to [LivePokeMap](https://play.livepokemap.com/), purchased separately from their service. Open **Feeds** and sign in with your own account on the phone. The feed lists remote Pokémon, stops, gyms, raids, and stations around your current location. Configure your friend code on the LivePokeMap website, then open **My Shinies** in Feed. My Shinies has its own 30-second refresh and is not limited by the Nearby radius. **Filters** provides species selection, IV/level ranges, size, always-show exceptions, PvP ranks, and shiny/background controls; My Shinies exposes the subset supported by its endpoint. Filter presets are saved locally. Rows show base-species sprites, gym team emblems, and raid bosses or eggs. Forms and shiny variants share the base-species image. Filter by category, radius, and minimum Pokémon IV; select a result for Map, Walk, or Teleport. The session token is stored encrypted with Android Keystore; the password is never saved. Logout removes the token. Refresh stops when the feed closes or the app leaves the foreground. Remote sightings can expire and are separate from locations loaded by the game. This uses LivePokeMap's observed web interface, which may change; access depends on your account and service availability.

**In-game Routes (experimental):** Open In-game Routes from the quick menu and choose a completion limit (1–50). PGRazor selects a nearby Route based on travel to its start plus its length, walks its recorded path, and waits for the game's reward confirmation before continuing. It can reverse reversible Routes and skips Routes already completed in that run. Starting closes the setup dialog. A draggable Routes icon appears while running; tap it for the current path and run status. It follows the overlay size setting. Keep the game in the foreground. Stop ends automated movement; finish or quit an unfinished Route in the game's Route menu before starting again. Adventure Sync prompts are automatically declined while location spoofing is enabled and the app has no Android location permission. Separate medal popups may still need dismissal. This is separate from GPX routes and Pokéstop Autowalk.

**Batch evolution:** In Settings, open Batch evolution, enter a Pokemon tag (for example `Evolve`), and preview before starting. The batch uses only that tag, reserves the available candy/items, and rechecks eligibility and stock before each evolution. Each selected Pokemon evolves once; no evolution screen is opened. Favorites, the current buddy, deployed Pokemon, eggs, Shadow/purified Pokemon, and ambiguous or conditional evolutions are skipped. Stop prevents further requests; an in-flight evolution may finish. No Lucky Egg is used automatically. Completed counts and XP come from server responses, and the engine waits for game inventory updates. Network uncertainty stops the batch without automatically retrying it. The user confirmed two successful evolutions after the inventory-reconciliation fix. Optional tag removal is off by default: enable it before preview to remove only the selected tag after each confirmed evolution. Skipped/failed entries and other tags are preserved. Removal errors stop the batch while retaining its evolution count; no automatic evolution retry occurs. The user confirmed evolution and optional tag removal working after the reference-write fix. Preview also validates request construction before any candy is consumed. Throughput testing remains pending.

**GPX routes:** Import a GPX file or choose a saved route in Autowalk. In the world map route builder, use GPX to import, save, or export routes. GPX routes preserve their point order and support up to 5,000 points, with optional looping. Importing does not start movement; press Start when ready. Saved routes stay on the device across app restarts and updates; export GPX files to keep a copy outside the app.

The map and cooldown history open full screen using the selected PGRazor theme. Select a map pin, then choose Walk here or Teleport. The manual route planner also offers Loop route (off by default). Map tiles retain their original colors.

The world map layers show locations currently loaded by the game near your avatar, not remote search results. Markers use bundled game artwork, with gym team-color rings and distinct raid categories. Tap a marker for live gym/raid details; overlapping markers open a chooser. Each location offers Walk and Teleport; teleporting first shows the destination cooldown confirmation. Egg badges identify unhatched raids. Small x/6 labels show gym defenders directly on the map; raid details resolve base-species names from bundled English game text. Unknown species keep their numeric fallback. Layer choices persist. Tile loading uses the [OpenStreetMap tile policy](https://operations.osmfoundation.org/policies/tiles/), including local caching and visible tiles only.

These are experimental client modifications. Server outcomes are not guaranteed. The AR+ option and local spawn-range changes do not prove server-side bonuses. Cooldown is a local estimate. Do not interpret a successful build as device validation.

**Settings backup:** Open **Settings > Export / import settings** to save a JSON file outside the app and restore it after reinstalling or on another phone. It includes feature preferences, speed, favorites, map layers, Feed filters/presets, and overlay positions. Login tokens, current location, cooldown history, and active routes are excluded. Import leaves VGP off. Ordinary updates signed with the same PGRazor key already preserve app data.

Join the [PGRazor Discord community](https://discord.gg/9QqfkDSQX3). You can also open it from the settings footer.

Enjoying PGRazor? You can support development on [Ko-fi](https://ko-fi.com/pgrazor). Donations are optional.

## Build and contribute

Development production builds use compiled native/Java gameplay and stripped native libraries. Frida is retained only for explicitly selected research builds; see [research tools](pgrazor/research/README.txt). Native metadata comes from validated JSON schemas, and production packaging rejects PGRazor JavaScript and Frida assets. This is unreleased branch work; published releases retain their own verification status.

- [Build, signing, release CI, and installation](docs/BUILDING.md)
- [Architecture and compatibility](docs/ARCHITECTURE.md)
- [Native migration verification and history](docs/NATIVE_MIGRATION.md)
- [Maintainability review and refactoring priorities](docs/MAINTAINABILITY.md)
- [Contributing and checks](CONTRIBUTING.md)
- [Third-party notices](THIRD_PARTY.md)

```powershell
python -B tools/check.py
```

Source CI runs device-free checks and compiles the Android controls. Its downloadable artifact contains **source and tools only**, not a playable APK. Playable releases are produced separately by the manually triggered **Playable APK release** workflow. See the [changelog](CHANGELOG.md).

## Repository layout

```text
.github/workflows/       Source CI and APK release workflow
docs/                    Build and architecture guides
pgrazor/src/      Android UI and control bridge
pgrazor/native/       Compiled gameplay runtime and native tests
pgrazor/art/          Compiled Android runtime bridges
pgrazor/runtime/      Optional research adapters
pgrazor/diagnostics/  Version-specific research probes
pgrazor/test/     Regression tests
pgrazor/vendor/   Pinned bridge, provenance, and licenses
tools/                   Checks, source packaging, release CI setup
website/                 React showcase deployed to GitHub Pages
```

PGRazor uses the `pgrazor/` project directory and `dev.pgrazor` Java namespace. Assets, settings, logs, build outputs, and tooling use the same name. There are no legacy-name aliases or settings migrations.

## License

Original PGRazor source is available under the [MIT License](LICENSE). Third-party components retain their own licenses. Pokémon GO and its assets are not covered by this license. PGRazor is an independent project and is not affiliated with the game publisher.
