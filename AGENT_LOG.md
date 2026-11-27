# AI working rules (all models) - effective 2026-11-24

ANY change added to the code MUST also use settings export/import and MUST satisfy the focused follow-up criteria below before merge:

1. P1 - Every new persisted key: add to SettingsBackupRules.SETTINGS allowlist AND a per-key validator consistent with persistence (string keys parsed as strings; numeric keys bounded exactly as read() bounds them). Add a full default+custom export->import round-trip test (empty/multiple/Unicode tags, invalid values). No new key may fall through to the generic Integer.parseInt / max=7 check. For future plugins, every new preference/overlay/map/feed key MUST be registered in PreferenceSchema (the single source of truth shared by SettingsBackup export and SettingsBackupRules import), never added to only one list.
2. P2 - Berry fallback: preserve the complete user fallback order and make BerryOverlay UI agree with native (resolve stocked candidates on the owner thread before limiting the 3-slot transport list). Regressions required: exhausted rule berries + stocked user priorities, partial stock, duplicates.
3. P2 - Rule/membership changes: invalidate stale VGP skip decisions by bumping the effective-rule revision when refreshed membership changes; distinguish temporary "tags not ready" (defer) from a definitive species rejection. Unchanged refreshes must not repeatedly clear the cache. Regression required: membership-only update with a still-live skipped spawn.

Source: focused Claude follow-up + coordinator validation of de777d2b91a81b53d905e3733e15c66c88262dbf.

## 2026-11-18 — Claude — Overlay tweaks (stage 1)
- Done: Buddy Emotion overlay is now a one-line badge (`♥ EP ★ ⏱Nm`, full text in content description) via `BuddyEmotionSettings.compactStatus()`; Pad cooldown timer shows local time as a small (0.75x) second line when both overlays are on, hiding the separate clock (`LocalTimeOverlay.embeddedLabel()`). Files: pgrazor/src/dev/pgrazor/{Pad,BuddyEmotionSettings,LocalTimeOverlay}.java.
- State: NOT compiled — `pgrazor\build.py` was blocked by session permissions. Not device-tested. Gym feed icon toggle already exists (Gym automation settings → "Show gym automation overlay").
- Next: run `tools\check.py` + `pgrazor\build.py`, device check; then stage 2 = Pokémon inventory export plugin (needs native inventory read design).

## 2026-11-18 19:10 — Claude — Stage 1 built and installed on S21
- Done: fixed Pad.java literal newline; added Gyms section checkbox "Show Gym feeder map icon" (SettingsPanel + GymAutomationSettings.setOverlayEnabled). pgrazor/build.py OK; nonroot run 20261118-190352-b9260ef8; release at pgrazor/build/release-new/PGRazor.apk (same cert fdc134f7… as installed) installed with adb install -r on SM-G991Q and launched.
- State: device behavior not yet confirmed by user. Bash builds need the .local/env.ps1 PATH set manually (devkitPro cmake/git trap). cleanup.ps1 retention warning is the known harmless one.
- Next: user checks overlays; then stage 2a/2b (pgrazor-pokemon-v1 export, reusing gym_runtime inventory enumeration) and 2c importer in pokematrix.

## 2026-11-18 — Claude — Smaller cooldown timer, VGP text toggle, double-tap choice
- Done: cooldown timer 11sp/128x30 (42 with clock). New prefs `vgpStatusText` (Go Plus page switch "Show VGP status text") and `mapTapTeleportNoAsk`; double-tap teleport now shows Walk/Teleport/Cancel with "Don't ask again" (TeleportDialog.confirmMapTap); walk+remember sets map tap mode to Walk, cycling map-tap mode clears the remembered choice. Keys added to SettingsBackupRules. tools/check.py OK, build OK, installed with adb install -r on S21 and launched.
- State: awaiting user device confirmation. Encounter count overlay already had a toggle (Encounter counter → "Show the count on the map").
- Next: stage 2 (Pokémon export).

## 2026-11-18 — Claude — Released v0.3.4.1 to user fork
- Done: version scheme v0.3.4.1 (4th component allowed in build.py + release.yml); commit 27f5d88 pushed to remote `fork` (hugoalves6/PGRazor_Pokematrix) main, tag v0.3.4.1, GitHub release with PGRazor.apk built with PGRAZOR_VERSION=v0.3.4.1 (same signing cert as installed).
- State: user asked not to install while in a raid — never install/restart on device without explicit go-ahead at that moment.
- Next: stage 2 Pokémon export (pgrazor-pokemon-v1) + pokematrix importer.

## 2026-11-20 — Claude — Rebase fork onto upstream v0.3.5
- Done: rebased fork commits on origin/main (v0.3.5 redesign); resolved Pad/SettingsBackupRules/CHANGELOG conflicts keeping upstream style (mono font, SettingsUi.warn) + our changes. Commit 810c438 adds v0.3.5.1 changelog/release notes. check.py + build.py pass; release APK built at pgrazor/build/release-new/PGRazor.apk (same cert). Backup branch backup-v0.3.4.1.
- State: not pushed (needs --force-with-lease), not installed. Not tested on device.
- Next: user approval to install on S21, then push + v0.3.5.1 release. Then Stage 2 (Pokémon export).
- 2026-11-20: pushed main (810c438) + tag v0.3.5.1 to fork; release published with PGRazor.apk; S21 install verified working.

## 2026-11-20 — Claude — TM shortcut buttons: RE findings (no code yet)
- Done: offline v39 metadata+libil2cpp dumper (scratchpad meta.py/dump.py; Niantic obfuscates Il2CppType data/kind, resolved via typedef byval map). Game TM flow = Niantic.Holoholo.UI.Items.ApplyRerollItemService.Factory (generic Factory<Data,IApplyRerollItemService,ApplyRerollItemService>).Create(Data); Data.ctor(Item, PokemonProto, AudioClip, Action<PokemonProto,HoloPokemonMove> onSuccess, onComplete, Action x3, Func<PokemonProto,bool> skipSuccessModal). Direct RPC alt: IItemBag.UseMoveRerollItemOnPokemon(item,pokemonId,useOnMove3). Current pokemon: PokemonInfoPanel.<PokemonProto>k__BackingField (iv_display already hooks DoUpdate).
- State: awaiting user decision on button placement (floating vs pinned to move rows).
- Next: implement native trigger + Java buttons; needs device install (ask first).

## 2026-11-20 — Claude — TM shortcut icons on Pokemon screen
- Done: native/tm_shortcut.{cpp,h} (rows via PokemonInfoPanel.moveSection movesGymRaid/movesCombat/AbilityN/Top/PowerCostArea/Base; TM flow via IApplyRerollItemServiceFactory.Create(Data)); hooks in iv_display.cpp (pgr_tm_note_panel), encounter_host.cpp tick, jni_core.cpp tmState/tmUse; TmShortcutOverlay.java + Pad; icons item-1201/1202 (provenance updated). Fix: keep 4 panel refs (game prebuilds neighbour pages) — release-tm2 verified working by user on S21. Settings toggle "TM shortcuts on the Pokémon screen" (key tmShortcuts, backup rules) built (build.py PASS), not yet on device.
- State: uncommitted; no review yet. Probe file removed.
- Next: user decision on move recommendations (proposed: extend pinned PvPoke snapshot with recommended movesets).

## 2026-11-20 — Claude — Tag catch (VGP+ catches only tagged candy families + rare, own berries)
- Done: new pgrazor/native/tag_catch.{h,cpp} (reads ITaggingRpcService tags every 60 s, species->family via IGameMasterData.Get_PokemonSettings familyId_/pokemonClass_, prefilter + judge); hooked in encounter_host.cpp (tick), smart_catch.cpp pgr_smart_plan (skip + berry override), vgp_controller.cpp discovery prefilter, jni_core.cpp (tagCatchConfig/tagCatchTags), CMakeLists (+ -fexceptions). Java: TagCatchSettings.java (settings group "Tag catch"), Control read/write, NativeCore, Pad TagCatchToggle (Pinap half + silver-tinted half; no Silver Pinap art on PokeMiners), backup rules/keys, overlay key tag-catch. Build OK: pgrazor/build/release-tags1/PGRazor.apk.
- State: NOT installed / device-unverified. Unverified field names: PokemonDisplayProto costume_/locationCard_ (missing -> not rare). Manual-catch berry override NOT implemented yet. native_harness fixtures (smart_catch_test, smart_catch_controller_test, encounter_host_test) don't link tag_catch/tm_shortcut -> would need stubs.
- Next: ask user before install; verify tag list loads, prefilter skips, Pinap/Silver Pinap thrown; then manual-catch berry; then CalcyIV/raid-team topics.

## 2026-11-20 23:00 — Claude — Tag catch device tests (release-tags2..8)
- Done: on device, tags read OK (6 tags/48 families/126 species); filter skips untagged and catches tagged (Poliwag). Fixed: 0/0/0 IVs counted as nundo -> treated as unknown; VGP caught everything before Java sent the config -> judge now skips until configured and while tags load (release-tags8, built, NOT installed).
- State: Smart Catch IVs always 0 at plan time, via the map proto, MapPokemon.get_Pokemon (virtual) and the encounter response; IV-based rules (tag rare + the original "shiny or IV>=90%") don't work. The IV display gets IVs elsewhere (iv_display.cpp hook_setup path), still to trace. tag_catch.cpp has temporary INFO diagnostics.
- Next: trace iv_display's encounter IV source; then move tag catch into VGP+ (the "Tags" category in PokemonSpeciesDialog, live tag:NAME entries in Catch only/Candy species, a rare berry option); then manual-catch berry.

## 2026-11-20 23:20 — Claude — IV panel behind 3D model (upstream bug) + TM dead refs
- Done: iv_panel.cpp draw_in_front refreshes nested canvas sortingOrder on every show (was set once; details screen opened from Pokedex drew the IV line behind the model). Device-verified on S21 with release-tags9. tm_shortcut.cpp drops destroyed panel refs (NullReferenceException spam). Issue text for upstream jpitty03/PGRazor drafted in chat (user files it).
- State: release-tags9 installed (includes tag catch + startup no-catch fix). VGP+ IVs still 0 at plan time (unresolved).
- Next: trace encounter IV source for Smart Catch; then move tag catch into VGP+.

## 2026-11-20 23:35 — Claude — Smart Catch encounter IVs fixed (device-verified)
- Done: pgr_encounter_pokemon (tag_catch.cpp) reads the map Pokemon's EncounterOutProto -> Pokemon -> Pokemon by fields (Shundo path); smart_catch.cpp pgr_smart_plan uses it (fallback to map proto). On S21 with release-tags10: real IVs (iv=15..35), filter judging correctly. Also fixes the original "shiny or IV>=90%" rule (upstream bug: the map proto has no IVs).
- State: tag_catch.cpp still has INFO diagnostics (encounter/judge/config lines) to remove before commit.
- Next: move tag catch into VGP+ (Tags category in PokemonSpeciesDialog, live tags in Catch only/Candy species, rare berry); manual-catch berry; possible upstream issue for the Smart Catch IV bug.

## 2026-11-20 23:55 — Claude — Tag rules moved into VGP+ (release-vgp1, built)
- Done: tag_catch.{h,cpp} now config v2 (active, alsoRare, rare mask/IV/berry, candy berry, ultra list, catch tags, candy tags); pgr_tag_catch_prefilter(state/species) + pgr_tag_rules(). smart_catch.cpp: Catch only = species list OR catch-tag families, "also rare" bypass, candy-tag berry, premium now uses rare criteria + rare berry (decide() runs with premium=0). vgp_controller prefilter updated. Java: TagCatchSettings = store + VGP+ UI block + tag picker; PokemonSpeciesDialog.showWithTags adds "Pokémon storage tags" category; SmartCatchSettings Candy/Catch only use it; old "Tag catch" settings group removed; migration from tagCatch* keys; backup keys renamed smartCatch*.
- State: built OK, NOT installed/tested. Defaults keep old premium behaviour (shiny|IV>=90%, Golden Razz). Manual-catch berry still TODO. native_harness fixtures not updated.
- Next: ask user to install; verify picker Tags entry, Catch only via tags, candy Pinap, rare Silver Pinap.

## 2026-11-21 00:00 — Claude — VGP integration audit + 2 upstream bugs (release-vgp2, built)
- Done: (1) vgp_controller resets the skip memory (POL->reset) when the tag-rule config revision changes (pgr_tag_catch_revision); before, skipped spawns stayed ignored for 1 h after a rule change. (2) GameLocationDelivery provider proxy feeds real fixes into Control.observeRealCoordinate. Upstream GitHub report: VGP stuck on "Waiting for game readiness" with spoofing off, because locationKnown() needed the Location getter hooks. Not reproduced here. (3) Power-up: DoUpdate isn't called, so the IV line kept the old level. iv_display.cpp now has pgr_iv_owned_refresh(panel), called from tm_shortcut watch_cp() when a noted panel's proto cp changes.
- State: release-vgp2 NOT installed/tested. Unknown whether the panel proto's cp changes after a power-up; if it doesn't, (3) won't fire. Audit gaps left: manual-catch berry; status text doesn't say "skipped (not tagged)"; SettingsSearch descriptions still old.
- Next: install; test power-up and the tag toggle; test spoofing off; then write the issues.

## 2026-11-21 00:15 — Claude — release-vgp2 verified
- Done: user confirmed on S21: power-up IV refresh (Drampa Lv16->16.5) and tag-toggle skip-memory reset. Spoofing-off VGP fix untested (cooldown). Three upstream issue texts written in chat.
- Next: manual-catch berry; remove leftover diagnostics; changelog/FORK.md; Codex review; commit only when the user asks.

## 2026-11-21 01:20 — Claude — CalcyIV export (release-calcy1, built)
- Findings: CalcyIV has "Import History from CSV" (Export/Backup screen). Import joins existing history, skips duplicates, and validates IVs against CP/HP/level ("invalid IV"). On-device test: rows with no CalcyIV internal IDs (Form, move IDs, ranks blank, move names only) are accepted. CSV header copied from the user's CalcyIV export (scratchpad).
- Done: tag_catch.cpp build_calcy() (owner tick; IPokemonBag.get_AllPokemon CopyTo; move names from the enum literals; level from the pvp_data cpm) + pgr_calcy_request/take; JNI calcyRows(Z); CalcyExport.java (CSV + SAF create via GpxDocuments.create(mime)); Settings › Tools › "Export Pokémon for CalcyIV".
- State: NOT installed/tested. Unverified: display field names gender_/alignment_, ShadowForm codes, regional forms (blank Form -> CalcyIV assumes default -> likely "invalid IV" rejects), move-name spelling differences (e.g. Mud-Slap).
- Next: install, export, import into CalcyIV, compare imported/failed counts.

## 2026-11-21 02:10 — Claude — Released v0.3.5.2
- Done: CHANGELOG v0.3.5.2, docs/releases/v0.3.5.2.md, FORK.md items 7-10; commit 9bcbe6d pushed to fork main, tag v0.3.5.2, GitHub release with PGRazor.apk (PGRAZOR_VERSION=v0.3.5.2). Excluded: .gitignore change, AGENT_LOG.md, docs/AI_CODEBASE_MAP.md.
- State: no independent Codex review was run. CalcyIV export ships untested ("experimental"). jadx decompile of CalcyIV runs in the background into the scratchpad (calcy/src).
- Next: analyse CalcyIV calc/gamestats/csv classes; build the native damage engine (best moves, raid counters).

## 2026-11-21 21:05 — Claude — RazIV shell (floating icon + panel)
- Done: CalcyIV engine analysed -> docs/raziv-spec.md (formulas, constants, test vectors). RazIV v0: native tm_shortcut.cpp `pgr_raziv_row` (row of the Pokemon page centred on screen, via new tag_catch `pgr_pokemon_row`, shared with CalcyExport), JNI `razivRow`, Java RazIvOverlay (icon assets/map-icons/raziv.png -> top panel: IV/stats; Moves view), Settings page "RazIV" in Play group, Control.raziv + backup keys. Built release-raziv1, installed on S21, game launched.
- State: not yet verified on device. No DPS/move stats yet; 3-view cycle (names/PvE/PvP) pending GameMaster move data. No Codex review.
- Next: user test; then read GameMaster moves (power/energy/duration, PvP, elite lists) for the moves panel.

## 2026-11-21 22:10 — Claude — RazIV move data
- Done: smart_battle.cpp load_tables also collects PvP move values (CombatMoveSettingsProto), damage window, species quick/cinematic/elite lists; `pgr_raziv_moves(proto)` returns S+F/C lines (flags current/elite/legacy); tm_shortcut appends them to the RazIV row. Java RazIvCalc (CalcyIV-shaped raid DPS: generic foe 200/200, enemy-DPS scaling 4, x0.5*Atk/70) + moves view (select 1 fast/2 charged, cycle Names/PvE/PvP, ★ elite, † legacy). Icon 54dp + UI scale. Built release-raziv2 (NOT installed; awaiting user OK).
- State: field names for combat moves/learn lists guessed with fallbacks (load_tables event reports pvp/learn/window flags). Raid-DPS absolute values not calibrated: offline check matched CalcyIV % (98/92/100) only with enemy scaling 1, not 4. Raw values logged under tag PGRazorRazIV.
- Next: install, open Oricorio, pull `adb logcat -s PGRazorRazIV`, calibrate against docs/raziv-spec.md vectors.

## 2026-11-21 22:55 — Claude — Rebased fork onto upstream v0.3.6 → v0.3.6.1
- Done: branch `update-v0.3.6`: commit RazIV WIP, `git rebase --onto v0.3.6 0993d10` (conflicts: SettingsBackupRules key lists via scratchpad merge_keys.py, CHANGELOG, CMakeLists -fexceptions list, SettingsPanel method seam). Test stubs fixed; 190/190 unit tests OK; build OK; release-v0.3.6.1 packaged (not installed). CHANGELOG/notes v0.3.6.1, FORK.md item 11 + heading "Fork changes".
- State: main still at old 9bcbe6d; nothing pushed. RazIV open: moves-line parse expects 13 fields (native sends 12) -> panel stuck "Loading"; move names raw "V0231movesplashfast" (HoloPokemonMove PascalCase); ~1% DPS gap.
- Next: user approval to install; fast-forward main to update-v0.3.6; push/tag only when asked.

## 2026-11-21 23:40 — Claude — Clean v0.3.6.1
- Done: on `update-v0.3.6`: removed CalcyIV export + GpxDocuments mime overload + old tagCatch* migration (4fa22e4); removed RazIV and the unverified spoofing-off fix (GameLocationDelivery) and their test stubs (d504a6c). RazIV kept intact on branch `raziv-wip` (for v0.3.6.2). CHANGELOG/notes/FORK.md describe only device-verified changes. 190/190 tests OK; build OK; pgrazor/build/release-v0.3.6.1/PGRazor.apk (not installed).
- Next: install on user OK; publish (force-push main, tag v0.3.6.1) only when asked.

## 2026-11-22 00:40 — Claude — Manual-catch berry from tag rules + freeze timing
- Done (uncommitted, branch update-v0.3.6): iv_display catch_begin -> tag_catch `pgr_tag_catch_note_encounter` computes rare/candy berry for manual encounters; encounter_host `with_rule_berry` puts it first in manual feeding; BerryOverlay shows it (JNI tagCatchManualBerry). Timing logs: PGRazorTagCatch "tag read took", PGRazorTm "TM tick took" (>16 ms). Menu dividers/spacing committed (abd7623). 190/190 tests; release-v0.3.6.1d built, not installed.
- Next: install; test manual catch of a candy-tag and a rare Pokémon; collect `adb logcat -s PGRazorTagCatch PGRazorTm` after freezes.

## 2026-11-24 - Debug/review session (shared AI log; blocks compacted)
- Device R5CR60GFM8E / SM-G991Q / Android 15. Installed APK SHA-256 2384b1407181a2d203efa94c... == pgrazor/build/release-rocket4/PGRazor.apk (HEAD 41c031e, built 15:47, installed 15:50): source == device, byte-identical.
- tools/check.py: 191 tests, only 4 pre-existing failures in test_native_prerequisites.py (temp git-repo detection). No new failures.
- BLOCK P1 settings export/import: Supposed fixed so far. Validators for smartCatchRareMask/IvMin/RareBerry/CandyBerry/UltraSpecies/CatchTags/CandyTags + round-trip test (defaults, custom, Unicode/multiple tags, invalids). test_live_feed_java.py OK.
- BLOCK P2a manual berry fallback: Supposed fixed so far. tag_catch.cpp packs only stocked rule berries on the owner thread; BerryPriority.fallback shared with BerryOverlay; new JVM test for exhausted/partial/duplicate/full/empty stock. test_tag_berry_fallback_java.py OK.
- BLOCK P2b tag membership revision: Supposed fixed so far. read_tags bumps revision on membership change, prefilter defers (2) while tags load, VGP reset gated by pgr_tag_catch_ready() via pure tag_catch_revision.h decision. Native harness test PASS on device.
- Verification: build.py Java/Android OK, native_build.py arm64 core OK, native_harness tag_catch_revision PASS on R5CR60GFM8E. NOT installed on device - ask before install/restart.
- BLOCK P2c live berry report (Drampa / rare): Supposed investigated so far - added TagBerryLiveBugSimulationTest (test_tag_berry_live_simulation_java.py) simulating installed vs fixed pipeline. Results: (1) no Silver Pinap + Golden Razz available -> BOTH pick Golden Razz, reported symptom NOT reproduced by selection logic; (2) all 3 rule berries out of stock -> OLD native feeds nothing while overlay shows user fallback (fixed by P2a); (3) default rare berry Golden Razz out of stock -> falls to user priority; (4) stale itemCatalog count -> overlay stuck on Silver Pinap while native feeds Golden Razz (UI/native mismatch). Need user config + device log to confirm which case is live.
- BLOCK P2c live berry bug (0 Silver Pinap / preview None): Supposed fixed so far. Simulation (TagBerryLiveBugSimulationTest, test_tag_berry_live_simulation_java.py) showed installed code does fall back to Golden Razz when stocked, but feeds nothing when all rules are unstocked; preview None comes from empty berry priorities. Found ID mismatch: tag-rule UI saved Silver Pinap as 708 while game assets/berry picker use 707 -> TagCatchSettings now uses 707 with 708->707 migration, validator accepts 707/708. New policy: if the configured rule berry is unstocked, drop baked-in Golden Razz/candy fallbacks and use the player 3-berry priority order (stock-gated). Simulation PASS; build.py + native_build.py arm64 OK; check.py 192 tests, only 4 pre-existing failures. NOT installed.
- BLOCK P2d dedicated fallback berry setting: Supposed fixed so far - new smartCatchFallbackBerries (up to 3 ordered picks) in TagCatchSettings VGP+ UI via generalized BerryPriorityPicker, backup key + validator, native config 11th field and Config.fallback. Used ONLY when the configured rare/candy berry is out of stock; otherwise the global 3-berry priority applies. Simulation scenarios 5/6 PASS (dedicated fallback wins; if also unstocked, global priority wins). build.py + native_build.py arm64 OK; check.py 192 tests, only 4 pre-existing failures. NOT installed.
- RELEASE v0.3.6.2 (2026-11-24): built with PGRAZOR_VERSION=v0.3.6.2 from input/com.nianticlabs.pokemongo_0.437.0.apkm; pgrazor/build/release-v0.3.6.2/PGRazor.apk SHA-256 d67ba15a7905911486b8cd7ed39894fec825090c3410970cd8e69a88c7b604a2. Same signer as installed (fdc134f7...). adb install -r on R5CR60GFM8E Success; re-pulled APK hash matches built exactly. CHANGELOG.md + docs/releases/v0.3.6.2.md written; fork main/tag/GitHub release with APK published.
- BLOCK P1b overlay backup import: Supposed fixed so far - SettingsBackup exported tag-catch.fx/fy but SettingsBackupRules rejected them (Unknown overlay setting). Added PreferenceSchema as single source of truth used by both export and import; future plugins register keys there once. Full-schema round-trip test + unknown-key rejection added. AI rule updated. build.py/native_build.py OK; check.py 192 tests, only 4 pre-existing failures. NOT installed/released.

- BLOCK P3 storage refresh: Supposed fixed so far - pgr_storage_invalidate() forces a fresh live-list read after every successful catch (shared_host hook_initialize), so a catch paired with a VGP transfer cannot leave a stale member for Rocket counters. Native build OK. NOT installed.
- BLOCK P4 full Pokemon JSON: Supposed implemented so far - PgrOwnedPokemon gains nickname, gender, shiny, costume and caughtMs; pgr_storage_json()/NativeCore.ownedPokemonJson() returns the full live storage as a JSON array (fresh read, consumers pick only fields they need). Rocket counter list carries the nickname as a 5th field and the card prints it to distinguish duplicates. Pure JSON escaping helper + native harness test PASS on R5CR60GFM8E; build.py/native_build.py OK; check.py 192 tests, only 4 pre-existing failures. NOT installed/released.
- BLOCK P5 live Rocket auto-pick + search text: Supposed implemented so far - hook_grunt synthesizes an empty BattleParty template when FindGruntParty returns null and returns counter_party 3 picks live (gated by smartBattle). Card shows a search: <nickname|name> cp<CP> line per counter; long-press copies all three terms joined by comma for the game Pokemon search. build.py/native_build.py OK; check.py 192 tests, only 4 pre-existing failures. DEVICE TEST REQUIRED before release: the synthesized party may be rejected by the game.
- RELEASE v0.3.6.3 (2026-11-25): built PGRazor.apk SHA-256 61c0561ddf20ca5580089a9402d12fd6327cb19c6a22f0bbae1e961de9ad9abf; installed on R5CR60GFM8E (Success, same cert fdc134f7...), pulled APK hash matches built exactly. Game reopened, UnityMainActivity foreground (pid 23163). logcat cleared; awaiting a grunt encounter to validate grunt-template / grunt-party / team events and the live 3-pick. NOT pushed to GitHub.
- DEVICE TEST 2026-11-25: v0.3.6.3 grunt #1 (saved party) auto-pick worked; grunt #2 (no saved party) crashed SIGSEGV right after the team event -> synthesized BattleParty template reverted. Stable v0.3.6.4 installed/reopened. v0.3.6.5-8 diagnostic builds installed: BattlePartyService methods enumerated at runtime (FindGruntParty, ConstructPartyAgainst, GetParty, SetParty...); GetRecommendedParty is NOT on that class. Likely declaring class BattlePrepUiBase (metadata neighbours), namespace still unresolved. User feedback: grunt #1 search text/pick correct (Mewtwo/Deoxys/Mewtwo incl CP); grunt #2 game party Landorus/Palkia/Machamp while card showed Rillaboom/Electivire/Electivire (one nicknamed) - clarify which side is which.
- 2026-11-25 continued: MuMu Chrome external-login report noted; partial GoogleBrowserAuth fallback edit was reverted to keep the tree stable (user asked to finish the Rocket work first). v0.3.6.9 installed with a BFS runtime class-graph probe (searching GetRecommendedParty from battle/party classes); APK verified to contain it. Device screen was Dozing / NotificationShade focused, so no probe log yet. Need the game foregrounded to collect it.

## 2026-11-25 end-of-day handoff (user asleep; install final update tomorrow)

Device state: R5CR60GFM8E was awake and unlocked. A background install of the diagnostic all-class-probe build may have finished; it is NOT the final build. User decision: final version label is v0.3.6.1a (not v0.3.7.0); compile and test TOMORROW with the user present. No more device actions or installs tonight.

Pending Rocket auto-pick (in order):
1. Read logcat for `recommended-probe` (tag PGRazor, phase smart-battle). v0.3.6.9 BFS probe scanned 200 classes with 0 hits. v0.3.6.1a enumerates every il2cpp assembly/class via il2cpp_domain_get / il2cpp_domain_get_assemblies / il2cpp_assembly_get_image / il2cpp_image_get_class_count / il2cpp_image_get_class and logs GetRecommendedParty's class, param count, param types and return type. Expect one line plus a `scanned/hits` summary.
2. Implement the real hook on that method: dynamic resolution via metadata API + debug_get_method_info (no pinned RVA), return counter_party's 3 picks; SILENT (no card message) when the pick is applied. Keep the card only as fallback if the hook is unavailable.
3. DELETE the probe and all diagnostic builds. Do not reuse the synthesized empty BattleParty approach: it crashed with SIGSEGV on a grunt with no saved party (v0.3.6.3, second grunt).
4. Build final version v0.3.6.1a, install with user present, test saved + unsaved grunt parties.

Pending MuMu Chrome login: user reported external browser does not open on MuMu Player and may fail on other devices. A partial GoogleBrowserAuth.begin() fallback edit was reverted at the user's request to finish Rocket first. Fix plan: resolve browser handlers with queryIntentActivities, prefer com.android.chrome, and add an in-app WebView fallback via GoogleAuthActivity (extra authUri, intercept custom-scheme redirect). Needs axml.py manifest entry unchanged; GoogleAuthActivity is already injected.

Uncommitted since v0.3.6.2 (commit 31c6f2b): PreferenceSchema + backup overlay fix, tag berry stock/fallback policy + dedicated fallback setting, tag revision updates, full Pokemon JSON (pgr_storage_json / NativeCore.ownedPokemonJson), storage invalidate on catch, searchable nickname+CP text, build.py unique intermediates, and the current smart_battle diagnostic probe. AGENT_LOG.md holds the compact block status.

Checks: build.py + native_build.py OK; check.py = 192 tests with only the 4 pre-existing test_native_prerequisites.py environment failures. Nothing pushed to GitHub after v0.3.6.2.

AI rule reminder: every new setting/preference key must be registered in PreferenceSchema and get a per-key validator + default/custom export->import round-trip test.


- 2026-11-25 version decision: final build must be PGRAZOR_VERSION=v0.3.6.1a. build.py release_version regex now accepts a letter suffix (e.g. v0.3.6.1a); verified locally. Compile/install/test tomorrow only.
- 2026-11-25 morning: hook implemented on Niantic.Holoholo.Gym.BattlePrepUiBase.GetRecommendedParty(int,bool) -> BattleParty (found by all-class il2cpp scan, 9632 classes). Dynamic hook via debug_get_method_info + pgrazor_hook; hook_grunt stores the line-up for unsaved grunts, hook_recommended returns counter_party result; card published only when the hook is unavailable (silent auto-pick). Probe removed. Final build v0.3.6.1a installed, same cert, game open. build.py + check.py both PASS (192 tests, no failures this run). Awaiting user grunt test (saved + unsaved).
- 2026-11-25 live test: GetRecommendedParty hook installed but game never called it on the tested grass grunt (no recommended-call/team event), so no pick and no card (card was wrongly suppressed). v0.3.6.1b: always publish the counter card as fallback; clear it only when the hook actually applies a party. Search text changed to species name + cp<CP> (filter: line); long-press copies all three terms. Installed on S21, game reopened. Awaiting user test of the fallback list.
- v0.3.6.1b checks: build.py exit 0, check.py exit 0 (192 tests). Awaiting user grunt card test.
- 2026-11-25 user confirmed v0.3.6.1b fallback card works (species + cp filter lines; long-press copies). Auto-pick via GetRecommendedParty hook is still unverified/non-functional; fallback kept for manual selection. NEXT SESSION: MuMu Chrome external-login fix (browser resolution preferring Chrome + in-app WebView fallback via GoogleAuthActivity); partial edit was reverted. Do not install anything until user is back.
- 2026-11-25 full-log diagnosis: saved grunt (character 23) produced grunt-enter -> party-build -> team (pick built) but game modal kept its own party; unsaved grunt (character 7) produced grunt-enter/grunt-party but NO recommended-enter: the base-class GetRecommendedParty hook was never called. v0.3.6.1d: hook EVERY class defining GetRecommendedParty(int,bool) via all-assembly enum, originals keyed by MethodInfo, same hook function; added 10s trace events (grunt-enter/construct-enter/recommended-enter/party-build). Full logcat recording to /sdcard/PGRazor/full-session.log; installed over WiFi adb 192.168.1.165:43951. Awaiting unsaved-grunt retest.
- v0.3.6.1d result: recommended-hook hooked=1 but no recommended-enter on unsaved grunt; the game did not call the hooked 2-arg method. v0.3.6.1e: discover ALL classes and hook GetRecommendedParty at arities 0..5; 2-arg uses the smart counter hook, other arities use a generic forwarding hook with trace(recommended-any-enter). Full logcat recording restarted; installed over WiFi. AWAITING unsaved grunt retest.
- 2026-11-25 ROLLBACK: method tracer (43+20 hooks) broke the game into a black loading loop; no method-enter events and no GetRecommendedParty call. Reverted smart_battle.cpp to HEAD (stable fallback card, species+cp filter). Built/installed v0.3.6.1g, game reopened. Auto-pick remains unsolved; do NOT re-add broad hooks without a safe design.
- v0.3.6.1h built: long-press on the Rocket counter card now copies ONLY the three CP numbers (comma-separated) instead of species+CP search terms. APK built; device was offline at install time, pending reinstall.
- 2026-11-26 v0.3.6.1i built: long-press copies CP-only list formatted as CP1234, CP2345, CP5678 (verified syntax: game ignores case/spaces, comma = OR). Device offline at install time; pending reinstall.

## 2026-11-26 new task: raid counter engine + settings audit

User request: build a raid-specific version of the smart-battle counter engine, organise the code into a complete reusable engine with user-facing switches/options, and audit that EVERY plugin added on top of upstream v0.3.6 is exported AND imported correctly; debug every added setting/option.

Plan in progress:
1. Two exploration agents are mapping (a) all persisted keys + export/import coverage and (b) battle/raid code decomposition. Do not duplicate their work until they report.
2. Baseline build.py + check.py running in background.
3. Device currently has v0.3.6.1i installed (CP-only long-press copy). smart_battle.cpp was rolled back to HEAD after the method-tracer loop; no experimental hooks remain.
4. Any new setting must be added to PreferenceSchema (shared by export/import), get a per-key validator in SettingsBackupRules, a default/custom round-trip test, and a UI toggle in the settings panel. Follow the AI rules at the top of this file.

No code changes for this task yet.

### Engine work completed (2026-11-26)
- Added pure `native/counter_engine.h` (shared rank/picks over Entry/Foe with HealingPolicy) and `counter_engine_test.cpp`; harness PASS on device.
- Fixed `battle_counter_test.cpp`: failure check moved after group_matchup assertions; harness PASS.
- Added settings `raidCounters` (bool, default true), `raidCounterTeamSize` (1-6, default 6), `raidCounterShowCard` (bool, default true). Persisted in Control.java, allowlisted + validated in SettingsBackupRules.java, round-trip/invalid tests added in SettingsBackupRulesTest.java, UI rows added in SettingsPanel.buildSmartBattle.
- Transport slot 21 now `F2|smart|raid|teamSize|showCard`; native `configured()` accepts F2 and legacy F1. Native globals drive raid overrides.
- `counter_party` now scores via the shared engine and accepts a size override. `hook_construct` uses raid team size and publishes a raid counter list when enabled.
- `publish_counters` publishes nickname as 5th field (fixes card mismatch) and uses raid team size for the raid card.
- Fixed gym feeder icon toggle to refresh the settings page summary.
- Added missing export/import tests for tmShortcuts, vgpStatusText, mapTapTeleportNoAsk.
- Verification pending after last Java edit; build.py + check.py running.
- 2026-11-26 v0.3.6.1j built and installed on R5CR60GFM8E (SHA in release build-info); game reopened, full logcat recording. Testing raid counter party + grunt saved-party path next.

## 2026-11-27 rebase onto upstream v0.3.8 (game 0.439.0) - DONE, device install pending

- Fetched upstream tags v0.3.7/v0.3.8. Direct `git rebase --onto v0.3.8 v0.3.6` hit conflicts in old v0.3.4.1 commits; aborted.
- Instead patched the whole fork delta onto a fresh branch `update-v0.3.8` with `git diff --binary v0.3.6 backup-v0.3.6.2-engine | git apply -3 --binary`. Resolved 6 conflicts: CHANGELOG (kept upstream v0.3.7/v0.3.8 + fork notes), CMakeLists (union of fexceptions sources), Pad (AutoRocket + vgpStatusText), SettingsBackupRules (union of allowlists), SettingsPanel (updateRow + RowList divider/invalidate), TeleportDialog (upstream skipTeleportConfirm + fork confirmMapTap).
- Feature audit of merged tree: all fork features present (tm shortcut, tag rules + revision, storage JSON, counter engine, raid settings, JNI methods, PreferenceSchema, berry fallback, telemetry tests).
- build.py PASS, check.py PASS: 196 tests. Committed `018ea7d Merge fork features onto upstream v0.3.8`; backup branch `backup-v0.3.6.2-engine` preserved.
- Downloaded pinned game bundle `input/com.nianticlabs.pokemongo_0.439.0.apkm` (SHA c6d3c04c38ca2f2446957633b4fee8c93dcc9cebdbbaddc9cb9651722ef8f54f) from APKMirror; `game_update.py verify-input` reports identity_verified=true.
- Built v0.3.8.1 with `--google-browser-login`: `pgrazor/build/release-v0.3.8.1/PGRazor.apk`, SHA 0b3a2552f71bb2c174af91421c0d06624eb2fa3bae4c03510cf855c41f516026, same signer fdc134f7.... Install blocked: device offline. Reconnect/authorise S21, then install + device test.
- 2026-11-27: v0.3.8.1 installed on S21 (game 0.439.0, versionCode 2026111602), game launched, full logcat recording restarted. Awaiting device test of raid counter party and saved-party grunt.

