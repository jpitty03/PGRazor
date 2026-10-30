// All site copy lives here so the components stay presentational and routine
// edits (wording, new settings, extra screenshots) never touch layout code.

export const links = {
  releasesLatest: 'https://github.com/jpitty03/PGRazor/releases/latest',
  releasesExperimental:
    'https://github.com/jpitty03/PGRazor/releases/tag/v0.1.7-experimental.1',
  github: 'https://github.com/jpitty03/PGRazor',
  discord: 'https://discord.gg/9QqfkDSQX3',
  kofi: 'https://ko-fi.com/pgrazor',
  cooldownReference: 'https://www.pgsharp.com/cooldown-rules/',
};

export const nav = [
  { href: '#features', label: 'Features' },
  { href: '#in-the-app', label: 'In the app' },
  { href: '#cooldown', label: 'Cooldown' },
  { href: '#get-started', label: 'Get started' },
];

export const heroFacts = [
  { term: 'Runs on', detail: 'ARM64 Android & MuMu — one APK' },
  { term: 'License', detail: 'MIT (original source)' },
];

export const features = [
  {
    title: 'No root, one APK',
    body: 'A single APK installs like a normal app on ARM64 Android phones and the MuMu emulator — no root needed.',
  },
  {
    title: 'Quick Catch',
    body: 'Shorten catch animations and skip the post-catch screens, so you can get back to the map.',
  },
  {
    title: 'Berry priorities',
    body: 'Pick up to three berries in priority order; each throw uses the first one you still have in stock.',
  },
  {
    title: 'IV & level display',
    body: 'Optionally show CP, level, and colour-graded IV right on the encounter and in Pokémon details.',
  },
  {
    title: 'Transfer keep rules',
    body: 'Auto-transfer low-IV catches while protecting shinies, legendaries, XXS, XXL, and a searchable custom species list. Favorites and eggs are always kept.',
  },
  {
    title: 'Virtual Go Plus',
    body: 'Catch nearby wild Pokémon with red balls only and spin nearby stops, with a target marker and an on/off status overlay.',
  },
  {
    title: 'Routes & favorites',
    body: 'A camera-relative joystick, saved places, Autowalk routes, speed presets, and a local cooldown estimate with history.',
  },
  {
    title: 'Hands-off automation',
    body: 'Optional gift opening and sending, buddy-berry feeding, and unlimited-incubator egg incubation by shortest or longest distance.',
  },
  {
    title: 'Seasonal events',
    body: 'An Events screen with per-event settings. For Halloween 2026, optionally open every visible map box and skip encounter screens. Experimental and off by default.',
  },
  {
    title: 'LivePokeMap feeds',
    body: 'Browse remote Pokémon, stops, gyms, and raids — this needs your own separate, paid LivePokeMap subscription.',
  },
  {
    title: 'Inventory limits',
    body: 'Choose which items to discard and how many to keep. The same limits apply to existing stock and new items.',
  },
  {
    title: 'Settings backups',
    body: 'Export and import your settings as JSON to move between phones or restore after reinstalling.',
  },
  {
    title: 'Scales to your screen',
    body: 'Resize the launcher, quick menu, joystick, and overlays to 100 / 75 / 50 / 25%, and drag them where you like.',
  },
];

// The "In the app" preview is a settings guide, not a live app. Each group maps
// to a real section of the in-app settings and lists actual control names.
export const settingGroups = [
  {
    id: 'catching',
    label: 'Catching',
    intro: 'Found under Catch & summary and Throws.',
    items: [
      { name: 'Quick Catch', detail: 'Shorten catch animations and skip the post-catch screens.' },
      { name: 'Throw rating', detail: 'Choose Nice, Great, or Excellent, with a separate always-curveball option.' },
      { name: 'Berry selector', detail: 'Choose up to three berries in priority order; off clears the list.' },
      { name: 'IV / level display', detail: 'Show CP, level, and colour-graded IV on the encounter and in details.' },
      { name: 'Transfer on catch', detail: 'Transfer new catches below 90% IV, with keep rules for shiny, legendary, XXS, XXL, and a custom species list.' },
      { name: 'Keep All overlay', detail: 'A draggable toggle that pauses auto-transfer without losing your rules.' },
    ],
  },
  {
    id: 'explore',
    label: 'Explore',
    intro: 'Found under Location & overlays.',
    items: [
      { name: 'Joystick & taps', detail: 'A camera-relative joystick with tap-to-walk, teleport, or double-tap teleport.' },
      { name: 'Speed presets', detail: 'Switch between 0, 9.3, 16, and 30 km/h.' },
      { name: 'Saved places & Autowalk', detail: 'Keep favorite locations and build looping routes through up to 50 nearby stops and gyms.' },
      { name: 'Nearby layers', detail: 'See stops, gyms, and raids the game has loaded around you, with team rings and raid categories.' },
      { name: 'Cooldown estimate', detail: 'A local timer with history — an estimate, not a guarantee.' },
      { name: 'LivePokeMap feeds', detail: 'Remote sightings, signed in with your own separate LivePokeMap subscription.' },
    ],
  },
  {
    id: 'automate',
    label: 'Automate',
    intro: 'Found under Virtual Go Plus, Eggs, Buddy, Gifts, Battles, and Events.',
    items: [
      { name: 'Virtual Go Plus', detail: 'Catch nearby wild Pokémon with red balls only and spin stops; starts off and stops when the app is backgrounded.' },
      { name: 'Auto incubate eggs', detail: 'Uses the unlimited incubator only, by shortest or longest distance.' },
      { name: 'Auto feed buddy', detail: 'Feed your buddy a berry you select.' },
      { name: 'Friend gifts', detail: 'Open and send gifts, with optional stickers.' },
      { name: 'Battles', detail: 'Assist with Team Rocket, raids, gyms, and Dynamax / Max encounters.' },
      { name: 'Events (Halloween 2026)', detail: 'Optionally open every visible map box and skip encounter screens. Experimental and off by default.' },
    ],
  },
  {
    id: 'personalize',
    label: 'Personalize',
    intro: 'Found across the interface and Location & overlays.',
    items: [
      { name: 'Light / dark themes', detail: 'The whole interface follows your chosen PGRazor theme.' },
      { name: 'Overlay scaling', detail: 'Size the launcher, quick menu, joystick, timer, and overlays at 100 / 75 / 50 / 25%.' },
      { name: 'Reposition overlays', detail: 'Drag the cooldown timer, VGP icon, and Keep All toggle anywhere.' },
      { name: 'Quick menu', detail: 'Reach Map, speed, Autowalk, Feeds, Favorites, Teleport, and Settings fast.' },
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
    caption: 'The PGRazor world map in MuMu, with saved places, routes, nearby layers, and Walk here / Teleport controls. Map data © OpenStreetMap contributors.',
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
    body: 'Open the latest release and download PGRazor.apk — not a source archive or a preview build.',
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
    a: 'One APK runs on ARM64 Android phones and the MuMu emulator. No root is required.',
  },
  {
    q: 'Can I install it over the Play Store app?',
    a: 'No. A store install is signed with a different certificate, so a PGRazor build can’t update it in place. Installing PGRazor replaces that copy instead of upgrading it.',
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
