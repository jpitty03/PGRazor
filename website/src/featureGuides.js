// Feature how-to copy. Keep IDs stable for shared guide links.
export const featureGuides = [
  {
    id: 'location-joystick',
    title: 'Location & joystick',
    category: 'Movement',
    summary:
      'Spoof your location, steer with a camera-relative joystick, and choose what a tap on the game map does.',
    where: 'Settings > Location & overlays, plus the on-screen joystick.',
    steps: [
      'Open Settings and expand Location & overlays.',
      'Turn on Location spoofing. It can run with Android’s system Location switch off or the app’s location permission denied.',
      'Use Coordinates & speed to set a start point and pick a speed preset: 0, 9.3, 16, or 30 km/h.',
      'Drag the joystick to walk; it steers relative to the game camera, not the screen.',
      'Set Game map tap to Do nothing, Walk, Teleport, or Walk / double tap teleport — the combined mode walks on one ground tap and teleports on two quick taps in the same spot.',
    ],
    notes: [
      'Zero speed pauses travel; selecting another speed resumes it.',
      'Show joystick toggles the handle; the panel behind it is intentionally transparent.',
      'Disabling spoofing returns the device to its normal location and stops simulated movement.',
      'Adventure Sync prompts are dismissed automatically only while spoofing is on and the app has no Android location permission.',
      'Spawn booster (80 m) is an optional, experimental Location & overlays toggle that widens the nearby spawn range; it appears only when the game supports it and does not prove a server-side bonus.',
    ],
  },
  {
    id: 'world-map',
    title: 'World map',
    category: 'Map',
    summary:
      'A full-screen map with place search, loaded nearby layers, a live location dot, and per-pin Walk or Teleport.',
    where: 'Quick Menu > Map (full screen, in your chosen PGRazor theme).',
    steps: [
      'Open the Quick Menu and tap Map.',
      'Search for a business, park, landmark, or address; results are biased toward the area you are viewing and selecting one centers the red destination pin.',
      'Toggle the nearby layers to show the stops, gyms, and raids the game has already loaded around your avatar.',
      'Tap a marker for live details; overlapping markers open a chooser.',
      'Choose Walk here or Teleport from a marker or the selected pin. Teleporting first shows the destination cooldown confirmation.',
    ],
    notes: [
      'Place search uses Photon / OpenStreetMap; results may be incomplete, and Walk and Teleport stay explicit actions.',
      'A live blue current-location dot is shown separately from the red destination pin.',
      'Layers show locations the game currently has loaded near you, not a remote search. Gym rings use team colors; raids show distinct categories and egg badges; small x/6 labels show gym defenders.',
      'Layer choices persist. Map tiles follow the OpenStreetMap tile policy with local caching of visible tiles; artwork is bundled game imagery and tiles keep their original colors.',
    ],
  },
  {
    id: 'hotspots-favorites',
    title: 'Hotspots & favorites',
    category: 'Map',
    summary:
      'Jump to 14 curated hotspots or your own saved places, with a cooldown estimate before any teleport.',
    where: 'Quick Menu > Hotspots or Favorites, and the World Map. Hotspots offers 14 curated destinations.',
    steps: [
      'Save a place from the World Map so it appears under Favorites (Favorites shares the map’s saved places).',
      'Open Favorites or the hotspot list from the Quick Menu.',
      'From the Quick Menu, pick Walk or Teleport. On the map, choosing a hotspot selects a waypoint instead.',
      'Confirm the destination cooldown estimate shown before a hotspot or favorite teleport.',
    ],
    notes: [
      '14 curated hotspots are available from both the World Map and the Quick Menu.',
      'Every user teleport — coordinates, Favorites, World Map pins, feeds, hotspots, and in-game map-tap teleport — shows the cooldown warning first.',
      'The cooldown figure is a local estimate, not a server countdown.',
    ],
  },
  {
    id: 'autowalk-gpx',
    title: 'Autowalk & GPX routes',
    category: 'Movement',
    summary:
      'Build a looping route through nearby stops and gyms, or import and save GPX paths to walk automatically.',
    where: 'Quick Menu > Autowalk and the World Map route builder.',
    steps: [
      'Open Autowalk from the Quick Menu to build a route through nearby loaded PokéStops and Gyms.',
      'Choose how many stops to include. The default is 50; you can raise it toward all currently loaded unique stops and gyms, or leave the count blank to select All.',
      'Turn on looping if you want the route to repeat; routes use direct segments between locations.',
      'For a recorded path, open the World Map route builder and use GPX to import, save, or export routes, then choose a saved route in Autowalk.',
      'Press Start when ready; importing a GPX file alone does not start movement.',
    ],
    notes: [
      'The displayed maximum follows nearby availability, so it changes as the game loads more stops and gyms.',
      'GPX routes keep their point order and support up to 5,000 points, with optional looping.',
      'Zero speed pauses a route; another speed resumes it. Backgrounding the app stops routes rather than resuming movement.',
      'Saved routes persist across restarts and updates; export GPX files to keep a copy outside the app.',
    ],
  },
  {
    id: 'in-game-routes',
    title: 'In-game Routes',
    category: 'Movement',
    summary:
      'Discover and walk the game’s own Routes automatically, waiting for each reward before moving on.',
    where: 'Quick Menu > In-game Routes (experimental).',
    steps: [
      'Open In-game Routes from the Quick Menu and choose a completion limit from 1 to 50.',
      'Start — PGRazor selects a nearby Route by travel to its start plus its length, then walks the recorded path.',
      'Let it wait for the game’s reward confirmation before continuing; it can reverse reversible Routes and skips Routes already completed in that run.',
      'Tap the draggable Routes icon for the current path and run status.',
      'Press Stop to end automated movement.',
    ],
    notes: [
      'Keep the game in the foreground. Finish or quit an unfinished Route in the game’s Route menu before starting again.',
      'This is separate from GPX routes and PokéStop Autowalk.',
      'Adventure Sync prompts are declined automatically while spoofing is on without Android location permission; separate medal popups may still need dismissal.',
    ],
  },
  {
    id: 'cooldown',
    title: 'Cooldown estimate',
    category: 'Reference',
    summary:
      'A local, draggable timer that estimates the soft-ban wait between location-sensitive actions.',
    where: 'Settings > Location & overlays (Cooldown overlay); tap the timer for history.',
    steps: [
      'Turn on the Cooldown overlay in Location & overlays.',
      'After a trigger action — a catch, throw, berry, spin, or gym action in range — read the estimated wait.',
      'Measure distance from your last location-sensitive action to the next; round up to the next listed distance on the chart.',
      'Tap the timer to open cooldown history; drag it to reposition it.',
    ],
    notes: [
      'The timer is a local estimate rephrased from community references; it does not read a server countdown and is not a guarantee.',
      'Time already spent waiting counts, and teleporting by itself does not restart the clock.',
      'Acting too early can cause fleeing catches or refused spins.',
      'Optional Stop on cooldown (estimated) can switch VGP off for the estimated wait.',
    ],
  },
  {
    id: 'catching',
    title: 'Catching & throws',
    category: 'Catching',
    summary:
      'Quick Catch, throw rating and curveball, encounter berry priorities, and on-encounter IV/level display.',
    where: 'Settings > Catch & summary and Settings > Throws.',
    steps: [
      'In Catch & summary, turn on Quick catch to shorten the catch animation and skip the post-catch screens.',
      'In Catch & summary, turn on Quick spin to let an opened PokéStop spin and close its reward screen automatically.',
      'In Throws, set Throw rating to Nice, Great, or Excellent, and optionally turn on Always curveball or Always hit.',
      'Turn on Catch IV/level to show CP, level, and color-graded IV above the encounter, and Inventory IV/level for owned Pokémon details.',
      'Tap the Catching berry overlay icon/count to choose up to three berries in priority order; each throw uses the first choice you still have in stock.',
    ],
    notes: [
      'Empty-stock berries can be selected; after three picks the next tap starts a new list, and Off clears it. Priorities are saved and included in settings backups.',
      'AR+ throw bonus and local spawn-range changes are experimental and do not prove a server-side bonus.',
      'Requested throw ratings combine with curveball but do not guarantee a catch.',
      'Some controls are gated by the game’s catch-summary and ball-animation support and may be hidden on unsupported versions.',
    ],
  },
  {
    id: 'transfer-rules',
    title: 'Transfer & keep rules',
    category: 'Catching',
    summary:
      'Auto-transfer low-IV catches while protecting shinies, legendaries, sizes, and a custom species list.',
    where: 'Settings > Catch & summary > Transfer on catch.',
    steps: [
      'Open Transfer on catch and turn on Transfer below 90% IV.',
      'Enable the keep rules you want: legendary/mythical/Ultra Beasts, shiny, event/costume, background, XXS, and XXL.',
      'Open Custom keep list to search and keep specific species (including all forms).',
      'Optionally turn on the Keep-all overlay button for a draggable toggle that pauses auto-transfer without losing your rules.',
    ],
    notes: [
      'Favorites and eggs are always kept.',
      'The rules apply to both manual and Virtual Go Plus catches.',
      'Turning Keep All off restores your existing rules; it does not enable auto-transfer if the main setting is off.',
      'The custom list and the overlay state are saved across restarts and included in settings backups.',
    ],
  },
  {
    id: 'virtual-go-plus',
    title: 'Virtual Go Plus',
    category: 'Automation',
    summary:
      'Catch nearby wild Pokémon with red balls only and spin nearby stops, with a target marker and status overlay.',
    where: 'Settings > Virtual Go Plus; start/stop from the VGP overlay icon.',
    steps: [
      'In Virtual Go Plus, turn on VGP overlay to show the overlay (the switch controls visibility, not the run state).',
      'Choose the VGP options you want: Nearby Pokémon, Nearby PokéStops, and Push notifications.',
      'Tap the overlay device icon to start or stop VGP; the ON/OFF label shows its state.',
      'Optionally enable Stop on cooldown (estimated) to switch VGP off for the estimated wait until you restart it.',
    ],
    notes: [
      'VGP catches with red balls only and starts off; it switches off when the app leaves the foreground.',
      'Catching, transfer, and protection rules follow your existing settings.',
      'VGP can keep working through in-game menus such as Pokémon storage using already-loaded spawns; a manual wild encounter still pauses it.',
      'Drag the VGP icon to reposition it; the status label follows the icon.',
    ],
  },
  {
    id: 'vgp-plus',
    title: 'VGP+',
    category: 'Automation',
    summary:
      'An expanded VGP mode (formerly Smart Background Catcher) with ball, berry, throw, and premium/candy rules.',
    where: 'Settings > Virtual Go Plus > VGP+; start/stop from the same VGP overlay icon.',
    steps: [
      'With the VGP overlay enabled, turn on VGP+ in the Virtual Go Plus section.',
      'Pick a Standard ball (Poké Ball, Great Ball, or Ultra Ball) and a Standard berry (Off, Razz, Nanab, Pinap, Golden Razz, or Silver Pinap).',
      'Optionally turn on “Shiny or IV ≥90%: Ultra Ball + Golden Razz”, and set High CP threshold (0–10000) and High level threshold (0–50) — a threshold of 0 disables that rule.',
      'Choose a Throw rating (Default/Nice/Great/Excellent) and optional Curveball.',
      'Open Candy species (Pinap) to select species that should get a Pinap for candy.',
      'Optionally enable Auto-spin Gym discs under VGP+. It defaults off and is separate from Nearby PokéStops; Gym Slotter and feeding need not be enabled.',
      'Use the normal VGP Start/Stop icon to run it.',
    ],
    notes: [
      'VGP+ attempts loaded, visible wild Pokémon without the standard VGP 40 m cutoff; the game still decides encounter eligibility and a declined encounter is skipped.',
      'Gym disc spinning runs on the map at loaded gyms of any team within 40 m, checks bag space, and respects each disc cooldown. The VGP start/stop and optional Stop on cooldown (estimated) controls apply. Gym-disc spinning has been user-verified.',
      'Premium rules take priority over candy rules. Empty selected stock falls back to available regular balls or skips the berry; Master Balls are never used.',
      'Throws wait roughly 1.2–1.8 seconds, with at most three additional throws after confirmed escapes.',
      'Existing storage, transfer, foreground, and manual-interaction gates still apply, and ratings do not guarantee a catch.',
    ],
  },
  {
    id: 'gym-slotter',
    title: 'Gym Slotter & Berry Feeder',
    category: 'Automation',
    summary:
      'Deploy a tagged defender to an open friendly gym slot and feed your gym defenders from one draggable overlay.',
    where: 'Settings > Raids, gyms & Dynamax > Gym Slotter & Berry Feeder.',
    steps: [
      'In the game’s Pokémon storage, open the Tags tab and create a tag named Gym (the #Gym shorthand is also recognized), then assign the eligible, healthy defenders you want deployed.',
      'Open Gym Slotter & Berry Feeder. Deployment and feeding are two independent switches that both default off, and VGP need not be running.',
      'Turn on the actions you want: “Auto-deploy to open Gym slots” and/or “Auto-feed berries to your Gym defenders”.',
      'For feeding, set the Nanab Berry reserve and Razz Berry reserve (0–1000, default 50); for deployment, optionally turn on the lowest-CP fallback.',
      'Enable “Show gym automation overlay”, then tap the draggable gym-and-berry icon — or the “Run Gym Slotter & Berry Feeder” checkbox — to start or stop the whole feature. Your deploy and feed choices are preserved.',
      'Keep the game on the map screen, where it validates each attempt. Deployment and feeding need a friendly gym within 40 m, with an open slot to deploy.',
    ],
    notes: [
      'The master starts off every session. Active cooldown stops the whole run and blocks a new start; restart it manually after the cooldown ends.',
      'The overlay uses the bundled gym symbol with a Razz Berry badge, shows ON, OFF, or CD, and carries a compact status line (for example Waiting for gym, Deploying defender, Feeding berries, or Cooldown).',
      'Tagged candidates have priority. The optional “lowest-CP fallback” is used only when no eligible #Gym defender is available and excludes shiny, lucky, and IV ≥90% Pokémon; every candidate skips favorites, the current buddy, injured, already-deployed, and otherwise ineligible Pokémon. The Gym tag is read for priority only and is not removed when a Pokémon is deployed.',
      'Feeding uses surplus Nanab then Razz above your reserves, with at least 1500 ms between confirmed feeds and bounded per-defender attempts.',
      'If readiness or eligibility changes before a request is sent, the feature waits and rechecks instead of remaining paused; this recovery was user-verified after two gym deployments on Android. After a temporary no-action result a nearby gym is rechecked promptly rather than skipped for about a minute, and reserve and retry status stay visible. Server refusals and inventory updates govern follow-up requests, and rewards are reported only from successful responses; an ambiguous timeout pauses the feature until you switch the options off. No dust or coin payout is promised.',
    ],
  },
  {
    id: 'buddy-emotion',
    title: 'Buddy Emotion Engine',
    category: 'Automation',
    summary:
      'Builds buddy mood with snapshots, petting, and reserve-aware berry feeding, replacing the old Auto feed buddy.',
    where: 'Settings > Buddy Emotion Engine.',
    steps: [
      'Expand Buddy Emotion Engine and turn on “Run the Buddy Emotion Engine”. This one master control covers snapshots, petting, hunger-based feeding, and optional gift collection.',
      'Open Feeding priority and tap berries in the order to try them; uncheck and reselect a berry to move it to the end. Use Clear to empty the list or Reset to restore Nanab, Razz, then Pinap — the default for new setups.',
      'Set Reserve per berry (0–1000, default 50); each feed uses the first selected berry whose stock is above its reserve.',
      'Optionally turn on “Open gifts your buddy brings back”.',
      'Use “Show EP overlay” to show or hide the draggable status display without stopping buddy care.',
    ],
    notes: [
      'It replaces the separate Auto feed buddy control. Existing single-berry choices are preserved, and legacy settings migrate with their old no-reserve behavior; disabling the engine grays the controls without clearing choices and does not re-enable the old feeder.',
      'Golden Razz and Silver Pinap are optional premium choices and are never selected by default; Poffins are never used. Clear the list to disable feeding.',
      'The overlay shows game-reported EP/excitement and an estimated maintenance countdown; the countdown is an estimate. Recent action history is scoped to the current account and buddy and survives restart.',
      'Walking and battles stay manual. Unknown request outcomes pause automation until you toggle it off.',
    ],
  },
  {
    id: 'batch-evolution',
    title: 'Batch evolution',
    category: 'Inventory',
    summary:
      'Preview a tagged batch, then evolve each eligible Pokémon once without opening the evolution cutscene.',
    where: 'Settings > Batch evolution (the button sits with the inventory controls, next to Auto-transfer items — there is no collapsible section for it).',
    steps: [
      'In Pokemon storage, open Tags, create a tag such as Evolve, and assign only the Pokemon you want evolved to it.',
      'Open Batch evolution and enter the exact tag name you want to evolve (for example, Evolve — this is just a sample tag).',
      'Optionally turn on “Remove this tag after each confirmed evolution” before previewing; it is off by default.',
      'Tap Preview batch to count eligible Pokémon and estimate candy/item cost before any candy is consumed.',
      'Tap Start batch, then confirm in the “Start batch evolution?” dialog to begin.',
      'Watch the running count; tap Stop (current evolution may finish) to end the batch.',
    ],
    notes: [
      'Only the one selected tag is used, and each selected Pokémon evolves once per batch; no evolution screen is opened.',
      'Editing the tag field or toggling the removal option clears a finished preview; Start re-enables only after you preview again and at least one Pokémon is eligible.',
      'Favorites, your buddy, deployed Pokémon, eggs, Shadow/purified Pokémon, and evolutions needing a branch choice or special condition are skipped. Other tags and skipped entries are preserved.',
      'Optional tag removal affects only the selected tag and only after each confirmed evolution; a removal error stops the batch while keeping its evolution count.',
      'No Lucky Egg is used automatically — start one yourself if you want it. Candy and items are rechecked before each evolution.',
      'Network uncertainty stops the batch without an automatic retry. If an outcome is unknown or an error occurs, do not blindly re-run it — inspect your storage first.',
    ],
  },
  {
    id: 'event-boxes',
    title: 'Event boxes (Halloween 2026)',
    category: 'Automation',
    summary:
      'An Events screen that can open every visible map box and skip its encounter screens.',
    where: 'Quick Menu > Events > Halloween 2026.',
    steps: [
      'Open the Quick Menu and tap Events, then Halloween 2026.',
      'Enable “Open boxes and skip encounters”.',
      'Keep the game on the map so it can open the boxes that are visible and collect their rewards.',
    ],
    notes: [
      'It opens every visible box and dismisses the Pokémon encounter screens; it does not catch those encounters.',
      'It cannot filter for candy-only boxes — it opens all visible boxes.',
      'Experimental and off by default.',
    ],
  },
  {
    id: 'inventory-cleanup',
    title: 'Inventory limits',
    category: 'Inventory',
    summary:
      'Choose which items to discard and how many to keep; the same limits apply to existing stock and new items.',
    where: 'Settings > Auto-transfer items.',
    steps: [
      'Open Auto-transfer items from the inventory controls.',
      'Select which items to discard and set a keep count for each.',
      'Let it apply the same limits to your current stock and to new items as they arrive.',
    ],
    notes: [
      'Full inventory scans stop when auto-discard is disabled; item settings request counts only while the panel is visible.',
      'VGP queries item counts and resumes after a confirmed cleanup, processing large batches incrementally.',
    ],
  },
  {
    id: 'friend-gifts',
    title: 'Friend gifts',
    category: 'Automation',
    summary:
      'Optionally open and send gifts, with an owned sticker added when available.',
    where: 'Settings > Friend gifts.',
    steps: [
      'Open Friend gifts.',
      'Turn on Auto open gifts and/or Auto send gifts.',
      'Optionally turn on “Add an owned sticker” to include a sticker you already have.',
    ],
    notes: [
      'Each toggle is independent and off by default.',
      'Buddy-brought gifts are handled by the Buddy Emotion Engine’s own gift option, separately from this section.',
    ],
  },
  {
    id: 'egg-incubation',
    title: 'Egg incubation',
    category: 'Automation',
    summary:
      'Auto-incubate using only the unlimited incubator, by shortest or longest distance.',
    where: 'Settings > Auto incubate eggs.',
    steps: [
      'Open Auto incubate eggs and turn on Auto incubate.',
      'Set Priority to shortest or longest distance.',
      'Stay on the game map; it starts another egg after the unlimited incubator becomes free.',
    ],
    notes: [
      'It uses only the unlimited incubator. Limited-use, Super, and timed incubators are not used.',
      'Walking and hatching eggs do not start a cooldown.',
    ],
  },
  {
    id: 'battles',
    title: 'Battles',
    category: 'Battles',
    summary:
      'Assist with Team Rocket, raids, gyms, and Dynamax / Max encounters.',
    where: 'Settings > Team Rocket and Settings > Raids, gyms & Dynamax.',
    steps: [
      'In Team Rocket, enable Auto quick attack, Auto charged attack, and/or Auto shield.',
      'In Raids, gyms & Dynamax, enable the per-type options you want: Raids / Gyms / Dynamax auto quick attack, auto charged attack, Dynamax auto Max attack, and per-type auto dodge.',
      'Start the battle in the game; the enabled assists act on the game’s natural callbacks.',
    ],
    notes: [
      'Automated attacks and dodges pause while the Pokémon switch picker is open and resume after it closes.',
      'Dynamax attack controls can recover from a missed setup callback so quick and charged attacks can still start.',
      'Experimental.3 adds gym next-opponent continuation: when Gyms: auto quick attack or Gyms: auto charged attack is on, the next-opponent prompt starts the following gym battle instead of waiting out its countdown. Leaving the battle manually or turning those gym attack options off cancels it. User-confirmed on MuMu.',
    ],
  },
  {
    id: 'livepokemap-feeds',
    title: 'LivePokeMap feeds',
    category: 'Feeds',
    summary:
      'Browse remote Pokémon, stops, gyms, raids, and stations — requires your own paid LivePokeMap subscription.',
    where: 'Quick Menu > Feeds.',
    steps: [
      'Subscribe to LivePokeMap separately, then open Feeds and sign in with your own account on the phone.',
      'Browse remote Pokémon, stops, gyms, raids, and stations around your current location.',
      'Open Filters to set species, IV/level ranges, size, always-show exceptions, PvP ranks, and shiny/background controls; filter presets are saved locally.',
      'Set your friend code on the LivePokeMap website, then open My Shinies in Feed for its own 30-second refresh.',
      'Select a result for Map, Walk, or Teleport.',
    ],
    notes: [
      'A separate, active, paid LivePokeMap subscription is required; it is not included.',
      'The session token is stored encrypted with Android Keystore and the password is never saved; Logout removes the token.',
      'Refresh stops when the feed closes or the app leaves the foreground. Remote sightings can expire and are separate from locations the game loads.',
      'This uses LivePokeMap’s observed web interface, which may change; access depends on your account and their service.',
    ],
  },
  {
    id: 'settings-backup',
    title: 'Settings, themes & backup',
    category: 'Interface',
    summary:
      'Light/dark themes, overlay scaling, a parent-off gating behavior for dependent controls, and JSON export/import.',
    where: 'Settings header (theme), Location & overlays (UI scale), and Export / import settings.',
    steps: [
      'Tap the theme icon in the Settings header to switch the whole interface between light and dark.',
      'In Location & overlays, set UI scale to 100%, 75%, 50%, or 25% to resize the launcher, quick menu, joystick, timer, and overlays.',
      'Drag the cooldown timer, VGP icon, and Keep All toggle to reposition them.',
      'Use Export / import settings to save a JSON file and restore it on another phone or after reinstalling.',
    ],
    notes: [
      'When a parent control is off, its dependent settings are grayed out but keep their saved choices (for example VGP+ under the VGP overlay, and the lowest-CP fallback under Auto-deploy).',
      'The backup includes feature preferences, speed, favorites, map layers, Feed filters/presets, and overlay positions.',
      'Login tokens, current location, cooldown history, and active routes are excluded, and import leaves VGP off.',
      'Account sessions are encrypted on-device and are never part of settings exports or Android backups.',
    ],
  },
  {
    id: 'accounts',
    title: 'Account profiles',
    category: 'Personalize',
    summary: 'Switch between remembered accounts while keeping their settings and locations separate.',
    where: 'Quick Menu > Switch account, or Settings > Accounts.',
    steps: [
      'Open Switch account and sign in normally to each account once to create its saved profile.',
      'Select a saved profile to switch. Movement and automation stop and the game restarts with the selected remembered session.',
      'If a session has expired, complete normal sign-in again. Check the selected account and location before starting automation.',
      'To remove a saved profile from this device, switch away from it first, then delete that inactive profile.',
    ],
    notes: [
      'Profiles keep separate feature settings, last location, cooldown history, favorites, and saved routes. New profiles start with defaults.',
      'Deleting a profile removes its locally saved sign-in and data; it does not delete the game account.',
      'Saved sessions are stored encrypted on this device and excluded from settings exports. Passwords are not saved.',
      'LivePokeMap authentication is shared; its filters are separate for each profile.',
    ],
  },
];
