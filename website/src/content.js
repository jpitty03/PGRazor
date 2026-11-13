// All site copy lives here so the components stay presentational and routine
// edits (wording, new settings, extra screenshots) never touch layout code.

export const links = {
  releasesLatest: 'https://github.com/jpitty03/PGRazor/releases/latest',
  releasesCurrent:
    'https://github.com/jpitty03/PGRazor/releases/tag/v0.3.0',
  github: 'https://github.com/jpitty03/PGRazor',
  discord: 'https://discord.gg/9QqfkDSQX3',
  kofi: 'https://ko-fi.com/pgrazor',
};

export const nav = [
  { href: '#features', label: 'Features' },
  { href: '?guide=all', label: 'Feature guides' },
  { href: '#cooldown', label: 'Cooldown' },
  { href: '#get-started', label: 'Get started' },
];

export const heroFacts = [
  { term: 'Runs on', detail: 'ARM64 Android & MuMu — one APK' },
  { term: 'Root option', detail: 'PGRazor Root for the official game (experimental)' },
  { term: 'License', detail: 'MIT (original source)' },
];

// The feature preview is a settings guide, not a live app. Each group maps
// to a real section of the in-app settings and lists actual control names.
export const settingGroups = [
  {
    id: 'catching',
    label: 'Catching',
    intro: 'Found under Catch & summary, Throws, and General Settings > IV Display.',
    items: [
      { name: 'Quick Catch', detail: 'Shorten catch animations and skip the post-catch screens.' },
      { name: 'Quick spin', detail: 'Let an opened PokéStop spin and close its reward screen automatically; gated by game support.' },
      { name: 'Throw rating & variety', detail: 'Choose Nice, Great, or Excellent, with a separate always-curveball option, or let Throw variety pick a rating at random for each throw.' },
      { name: 'Auto throw', detail: 'Off by default. Throws for you in encounters with your throw settings, selected ball and berry. Reward Pokémon from Go Pass and research are left for you to throw.' },
      { name: 'Catch on map', detail: 'With Auto throw on, tap a wild, lure, or incense Pokémon to catch it without opening the encounter. A pin marks the one being caught, and a tap made while VGP+ is busy waits for it.' },
      { name: 'Berry selector', detail: 'Choose up to three berries in priority order from the encounter overlay or from Settings; off clears the list.' },
      { name: 'Catch chance & Auto ball', detail: 'See each ball’s chance from the server’s catch rate, and optionally let Auto ball pick the cheapest Poké, Great, or Ultra Ball that reaches your target. Never a Master Ball.' },
      { name: 'IV display', detail: 'Show level, IVs, and optional PvP league ranks on the encounter, the Pokémon stats screen, and the storage list.' },
      { name: 'Transfer on catch', detail: 'Transfer new catches below 90% IV, with keep rules for shiny, legendary, XXS, XXL, 0% IV, good PvP IVs, and a custom species list.' },
      { name: 'Evolve before transferring', detail: 'Evolve new catches once or through every available stage before they are transferred, for your transfer rules or a separate species list.' },
      { name: 'Keep All overlay', detail: 'A draggable toggle that pauses transfer for manual encounters without changing VGP or VGP+ rules.' },
    ],
  },
  {
    id: 'explore',
    label: 'Explore',
    intro: 'Found under General Settings, with the map and hotspots in the Quick Menu.',
    items: [
      { name: 'Joystick & taps', detail: 'A camera-relative joystick with tap-to-walk, teleport, or double-tap teleport.' },
      { name: 'Speed presets', detail: 'Switch between 0, 9.3, 16, and 30 km/h.' },
      { name: 'Saved places & Autowalk', detail: 'Keep favorite locations and build looping Autowalk routes; the default is 50 nearby stops and gyms, and you can raise it toward all currently loaded or leave it blank for All.' },
      { name: 'Place search & hotspots', detail: 'Includes place search, a live blue location dot, and 14 curated hotspots, each with a cooldown estimate before teleport.' },
      { name: 'Nearby layers', detail: 'See stops, gyms, and raids the game has loaded around you, with team rings, raid categories, and darkened Team GO Rocket stops; choose Walk or Teleport from a pin.' },
      { name: 'Spawn booster (80 m)', detail: 'An optional, experimental General Settings toggle that widens the nearby spawn range; it does not prove a server-side bonus. It can cause lag in busy areas with many nearby Pokémon, so turn it off if the game stutters.' },
      { name: 'Cooldown estimate', detail: 'A local timer with history — an estimate, not a guarantee.' },
      { name: 'Local time', detail: 'An optional draggable clock showing the time at your spoofed location.' },
      { name: 'Feeds', detail: 'Remote sightings from LivePokeMap (your own separate subscription) or PoGoCoordinates (Discord sign-in or guest), with IV, level, and PvP rank filters.' },
    ],
  },
  {
    id: 'automate',
    label: 'Automate',
    intro: 'Found under Virtual Go Plus, Eggs, Buddy Emotion Engine, Gifts, Team Rocket, Gyms, Raids & Dynamax, inventory controls, Events, and Research. Automation groups tasks you start manually, including Auto-Raid and Shundo Sniper.',
    items: [
      { name: 'Virtual Go Plus', detail: 'Catch nearby wild Pokémon with red balls only and spin stops; starts off and stops when the app is backgrounded.' },
      { name: 'VGP+', detail: 'Includes ball, berry, throw, premium/candy, and auto ball rules, an optional catch-only species list, catches incense Pokémon too, reaches loaded Pokémon past the 40 m cutoff, and optionally spins nearby gym discs through the VGP controls.' },
      { name: 'Auto-Raid', detail: 'Finds the nearest matching raid the game has loaded, travels there, waits for enough players, battles with your raid auto-attack settings, and attempts the bonus catch. Remote Raid Passes are never used.' },
      { name: 'Shundo Sniper (experimental)', detail: 'Teleports to 100% IV Pokémon and checks each for shiny, holding a shiny until your cooldown ends. Constant teleporting raises the risk of a warning or ban; use an alt account.' },
      { name: 'Auto incubate eggs', detail: 'Uses the unlimited incubator only, by shortest or longest distance.' },
      { name: 'Buddy Emotion Engine', detail: 'Includes snapshots, petting, and reserve-aware berry feeding with optional gift collection; it replaces Auto feed buddy.' },
      { name: 'Gym Slotter & Berry Feeder', detail: 'Deploys Gym-tagged defenders to open friendly gym slots and feeds defenders; two independent toggles, both off by default, run from one draggable overlay.' },
      { name: 'Daily Incense Session', detail: 'A manually started walk for up to 15 minutes. Loaded Galarian bird detection stops walking and VGP, attempts to open the encounter, and alerts you to catch manually. The live bird handoff is not yet verified.' },
      { name: 'Send & Open Gifts', detail: 'Run one gift batch with confirmed sent/opened counts and a Stop button. Optional full-bag opening uses the game’s reward setting; eligibility and daily limits still apply.' },
      { name: 'Remote Gym Feeding', detail: 'Run one batch for gyms you defend, using surplus Nanab, Razz, and optional Pinap above your reserves. Stop cancels unsent work; game limits still apply.' },
      { name: 'Batch evolution', detail: 'Previews a tagged batch, then evolves each eligible Pokémon once without the cutscene; no automatic Lucky Egg. Find it as Mass Evolve under Settings or Quick Menu > Automation.' },
      { name: 'Friend gifts', detail: 'Open and send gifts, with optional stickers.' },
      { name: 'Battles', detail: 'Assist with Team Rocket, raids, gyms, and Dynamax / Max encounters; in Rocket battles the next Pokémon is picked automatically when one faints. Battle until defeated repeats gym rounds, and Max energy orbs are collected during Max battles.' },
      { name: 'Smart battling', detail: 'Fill your team with counters before gym, raid, and Rocket battles, and swap to your best counter during gym and Rocket battles.' },
      { name: 'Smart healing', detail: 'Heal damaged Pokémon after battles, periodically, or both, with the potion and revive mix that wastes the least — a Potion for 5 missing HP, not a Max Potion.' },
      { name: 'Research', detail: 'Auto-claim completed field research, delete the tasks you tick, and catch waiting research encounters without the encounter screen.' },
      { name: 'Events', detail: 'Go Wild November 2026 adds default-off Safari Ball preferences for Mighty Pokémon in VGP+ and Auto ball. The Halloween 2026 box opener is still listed for when its boxes are on the map.' },
    ],
  },
  {
    id: 'personalize',
    label: 'Personalize',
    intro: 'Found across the interface and General Settings.',
    items: [
      { name: 'Light / dark themes', detail: 'The whole interface follows your chosen PGRazor theme.' },
      { name: 'Overlay scaling', detail: 'Size the launcher, quick menu, joystick, timer, and overlays at 100 / 75 / 50 / 25%.' },
      { name: 'Reposition overlays', detail: 'Drag the cooldown timer, local time clock, VGP icon, and Keep All toggle anywhere.' },
      { name: 'Quick menu', detail: 'Reach Map, speed, Autowalk, In-game Routes, Hotspots, Feeds, Favorites, Teleport, Automation, Research, Events, Switch account, and Settings fast. Customize Quick Menu hides entries you don’t use; Settings always stays visible.' },
      { name: 'Skip cutscenes', detail: 'Bypass evolution, hatch, new-species, power-up, level-up, feature-unlock, medal, and Team GO Rocket cutscenes and result screens so they are never drawn. Battles still start only when you press Battle.' },
      { name: 'Battery usage', detail: 'Lower resolution, a 20 FPS limit, or low graphics quality to save battery and keep the phone cooler.' },
      { name: 'Hide overlay', detail: 'Make the game look stock: hide every PGRazor button, status, and IV text; tap with three fingers to bring them back.' },
      { name: 'Settings backup', detail: 'Export / import settings as JSON; tokens, location, and routes are left out.' },
    ],
  },
];

// Real screenshots bundled in public/images. Dimensions are the source pixels,
// used for width/height to avoid layout shift while loading.
export const screenshots = [
  {
    id: 'world-map',
    src: 'images/world-map.png',
    title: 'World map',
    caption: 'The PGRazor world map, with saved places, routes, nearby layers, and Walk here / Teleport controls. Map data © OpenStreetMap contributors.',
    width: 1080,
    height: 1920,
  },
  {
    id: 'encounter',
    src: 'images/encounter.png',
    title: 'Encounter overlays',
    caption:
      'A Pokémon GO encounter with the optional CP / level / IV read-out above the Pokémon, the berry count, the transfer shield, and the VGP status indicator — all drawn over the game.',
    width: 1080,
    height: 2404,
  },
  {
    id: 'main-menu',
    src: 'images/main-menu.png',
    title: 'Settings menu',
    caption:
      'The full-screen settings menu in dark theme, grouped into collapsible sections for location, catching, automation, and more, with a Discord link in the footer.',
    width: 1080,
    height: 2404,
  },
];

export const steps = [
  {
    n: '01',
    title: 'Download',
    body: 'Open the latest release and download PGRazor.apk — not a source archive, and not the Root files unless your device is rooted.',
  },
  {
    n: '02',
    title: 'Install',
    body: 'Open the APK on an ARM64 Android phone or in the MuMu emulator, allowing installs from your file manager when prompted.',
  },
  {
    n: '03',
    title: 'Open',
    body: 'Launch Pokémon GO, tap the small R icon, then choose Settings for the full controls.',
  },
];

export const faqs = [
  {
    q: 'What does it run on?',
    a: 'One APK runs on ARM64 Android phones and the MuMu emulator. No root is required. Rooted devices can use the experimental PGRazor Root installer with the official game instead.',
  },
  {
    q: 'Can I install it over the Play Store app?',
    a: 'No. A store install is signed with a different certificate, so a PGRazor build can’t update it in place. Installing PGRazor replaces that copy instead of upgrading it.',
  },
  {
    q: 'What is PGRazor Root?',
    a: 'A separate, experimental installer for rooted devices. It loads PGRazor into the official Pokémon GO app through a Zygisk module, so the game’s own APK and signature stay untouched. It needs Magisk 27 or newer, KernelSU, or APatch, and the exact game version each release supports.',
  },
  {
    q: 'Is LivePokeMap included?',
    a: 'No. The remote feeds need your own active LivePokeMap subscription, purchased separately from their service.',
  },
  {
    q: 'Does it guarantee catches or prevent bans?',
    a: 'No. These are experimental client modifications and server outcomes aren’t guaranteed. The cooldown timer is only a local estimate.',
  },
  {
    q: 'Will it work on any game version?',
    a: 'Native hooks are built for a specific Pokémon GO version, so a different game version can leave features unavailable. Check each release’s notes.',
  },
];

// Community cooldown guidance. Prose is rephrased from player references, kept
// short, and framed as estimates — not an official server guarantee. See the
// Cooldown component for how trigger / safe groups and the chart are rendered.
export const cooldown = {
  intro:
    'Measure the distance between your last location-sensitive action and the next one. Time already spent waiting counts; teleporting alone does not restart the clock. The community chart below reaches a two-hour maximum. Acting too early can cause fleeing catches or refused spins. PGRazor estimates the wait locally; it does not read a server countdown.',
  // Actions widely reported to start the clock.
  triggers: [
    'Wild catches, including incense and lure spawns',
    'Throwing or dropping any ball',
    'Feeding an encounter berry, raids included',
    'Spinning stops, even past the daily limit',
    'Gym actions in radar range: placing, battling, feeding',
    'A flee caused by the catch limit',
    'Automated catching or spinning',
  ],
  // Actions widely reported not to start the clock.
  safe: [
    'Teleporting by itself',
    'Opening an encounter without a ball or berry',
    'Walking or hatching eggs',
    'Remote gym feeding from outside radar range',
    'Claiming quest or weekly rewards; catching quest rewards',
    'Gifts, trades, evolves, power-ups, transfers, snapshots',
    'Buddy changes and candy',
    'Pokémon items or a second charged move',
    'Incense, Lucky Egg, or Star Piece activation',
    'Inventory deletion',
    'PvP and trainer battles',
    'Placing lures',
    'Shop and avatar changes',
    'Switch transfers, free passes, photobombs',
    'Speed raids only after the prior cooldown ends, without throwing a ball or feeding a raid encounter berry',
  ],
  // Distance (km) → community-reported wait. Values are estimates; round up to
  // the next listed distance. "<" means reported as less than that many minutes.
  chart: [
    { km: '1', wait: '<1 min' },
    { km: '2', wait: '1 min' },
    { km: '3', wait: '<2 min' },
    { km: '5', wait: '2 min' },
    { km: '7', wait: '5 min' },
    { km: '9', wait: '<7 min' },
    { km: '10', wait: '7 min' },
    { km: '12', wait: '8 min' },
    { km: '18', wait: '10 min' },
    { km: '26', wait: '15 min' },
    { km: '42', wait: '19 min' },
    { km: '65', wait: '22 min' },
    { km: '76', wait: '<25 min' },
    { km: '81', wait: '25 min' },
    { km: '100', wait: '35 min' },
    { km: '220', wait: '<40 min' },
    { km: '250', wait: '45 min' },
    { km: '350', wait: '<51 min' },
    { km: '375', wait: '54 min' },
    { km: '460', wait: '62 min' },
    { km: '500', wait: '<65 min' },
    { km: '565', wait: '69 min' },
    { km: '700', wait: '78 min' },
    { km: '800', wait: '84 min' },
    { km: '900', wait: '92 min' },
    { km: '1000', wait: '99 min' },
    { km: '1100', wait: '107 min' },
    { km: '1200', wait: '114 min' },
    { km: '1300', wait: '117 min' },
    { km: '1350+', wait: '120 min' },
  ],
  reviewed: '2026-10-29',
};
