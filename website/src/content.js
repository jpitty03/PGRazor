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
// to a heading on the in-app Settings home screen and lists actual page and
// control names.
export const settingGroups = [
  {
    id: 'play',
    label: 'Play',
    intro: 'The six pages under Play on the Settings home screen, plus the Quick Menu. Type in Search settings at the top of Settings to jump straight to any setting.',
    items: [
      { name: 'Location & Map', detail: 'Location spoofing, Coordinates & speed (0, 9.3, 16, or 30 km/h), the World map, a camera-relative joystick, and Idle walking. Game map tap walks, teleports, or does both with a double tap. Also here: the experimental Spawn booster (80 m), which can cause lag in busy areas; the Cooldown overlay and history, a local estimate rather than a guarantee; Show local time; and an encounter counter.' },
      { name: 'Catching', detail: 'Quick catch skips the post-catch screens. Throws sets a Nice, Great, or Excellent rating or Throw variety, with Always curveball, Always hit, and the experimental AR+ throw bonus. Show catch chance and Auto ball use the server’s catch rate and never pick a Master Ball. Auto throw and Catch on map throw for you, and Berries sets up to three berries in priority order.' },
      { name: 'Pokémon & Transfer', detail: 'Transfer new catches below 90% IV, with Always keep rules for shiny, legendary, event, costume, background, XXS, XXL, and 0% IV, a custom keep list, a Keep-all overlay button, and Keep for PvP by league rank. Also Evolve before transferring, IV display for the encounter, stats screen, and storage list, and Mass Evolve under Tools.' },
      { name: 'Go Plus, Stops & Items', detail: 'Virtual Go Plus catches nearby wild Pokémon with red balls only and spins stops; it starts off and stops when the app is backgrounded. VGP+ adds ball, berry, throw, candy, and auto ball rules, catches incense Pokémon, and can spin nearby gym discs. Also Quick spin for PokéStops, Exchange gifts with Mateo on Routes, and Auto-transfer items limits.' },
      { name: 'Battles', detail: 'All battles holds Smart battling, which fills your team with counters and swaps during gym and Rocket battles, and Smart healing, which uses the potion and revive mix that wastes the least. Team Rocket, Gyms, Raids, and Dynamax each have their own attack options. Gyms adds Gym Slotter & Berry Feeder, Battle until defeated, and Remote Gym Feeding; Raids adds Auto-Raid.' },
      { name: 'Buddy, Eggs & Friends', detail: 'The Buddy Emotion Engine builds buddy mood with snapshots, petting, and reserve-aware berry feeding. Eggs auto-incubates with the unlimited incubator only, by shortest or longest distance. Friend gifts opens and sends gifts, with optional stickers, and starts Send & Open Gifts.' },
      { name: 'Quick Menu', detail: 'Tap the R icon for Map, Walk, Autowalk, In-game Routes, Feeds, Favorites, Hotspots, Teleport, Switch account, Events, Automation, Research, Hide overlay, and Settings. The map adds place search, nearby layers, and 14 curated hotspots, each teleport with a cooldown estimate first. Feeds shows remote sightings from LivePokeMap (your own separate subscription) or PoGoCoordinates.' },
    ],
  },
  {
    id: 'automation',
    label: 'Automation',
    intro: 'The Automation and Events pages. Each one-shot task runs only when you start it; most are also in Quick Menu > Automation.',
    items: [
      { name: 'Shundo Sniper (Experimental)', detail: 'Teleports to 100% IV Pokémon and checks each for shiny, holding a shiny until your cooldown ends. Constant teleporting raises the risk of a warning or ban; use an alt account.' },
      { name: 'Shiny Scanner (Experimental)', detail: 'Checks wild Pokémon within 80 m for shinies without catching them. Each check uses one encounter; speed, species, and a 24-hour encounter limit are adjustable. It pauses while VGP+, Shundo Sniper, or Auto-Raid runs.' },
      { name: 'Auto-Raid', detail: 'Finds the nearest matching raid the game has loaded, travels there, waits for enough players, battles with your raid auto-attack settings, and attempts the bonus catch. Remote Raid Passes are never used.' },
      { name: 'Daily Incense Session', detail: 'A manually started walk for up to 15 minutes. Loaded Galarian bird detection stops walking and VGP, attempts to open the encounter, and alerts you to catch manually. The live bird handoff is not yet verified.' },
      { name: 'Remote Gym Feeding', detail: 'Run one batch for gyms you defend, using surplus Nanab, Razz, and optional Pinap above your reserves. Stop cancels unsent work; game limits still apply.' },
      { name: 'Send & Open Gifts', detail: 'Run one gift batch with confirmed sent/opened counts and a Stop button. Optional full-bag opening uses the game’s reward setting; eligibility and daily limits still apply.' },
      { name: 'Mass Evolve', detail: 'Previews a tagged batch, then evolves each eligible Pokémon once without the cutscene; no automatic Lucky Egg.' },
      { name: 'Research', detail: 'Under Field research, and in Quick Menu > Research. Auto-claim completed field research, delete the tasks you tick, and catch waiting research encounters without the encounter screen.' },
      { name: 'Events', detail: 'Under Event settings, and in Quick Menu > Events. Go Wild November 2026 adds default-off Safari Ball preferences for Mighty Pokémon in VGP+ and Auto ball. The Halloween 2026 box opener is still listed for when its boxes are on the map.' },
    ],
  },
  {
    id: 'app',
    label: 'App',
    intro: 'The Display & Performance and Accounts & Backup pages, plus settings search and overlay controls.',
    items: [
      { name: 'Settings search', detail: 'Type part of a setting’s name or description, or a keyword such as “fake gps” or “light mode”, in Search settings at the top of Settings. Tap a result to open its page and scroll to it.' },
      { name: 'Interface', detail: 'Dark theme switches the whole interface between light and dark, as does the theme icon on the Settings home screen. UI scale sizes the launcher, quick menu, joystick, timer, and overlays at 100 / 75 / 50 / 25%.' },
      { name: 'Skip cutscenes', detail: 'Bypass evolution, hatch, new-species, power-up, level-up, feature-unlock, medal, and Team GO Rocket cutscenes and result screens so they are never drawn. Battles still start only when you press Battle.' },
      { name: 'Battery usage', detail: 'Lower resolution, a 20 FPS limit, or low graphics quality to save battery and keep the phone cooler.' },
      { name: 'Quick menu entries', detail: 'Hide Quick Menu entries you don’t use. This changes visibility only, not whether a feature is on; Settings always stays visible.' },
      { name: 'Account & data', detail: 'Accounts switches between remembered accounts with separate settings. Export / import settings saves a JSON backup without tokens, location, or routes. Run log / export opens the log.' },
      { name: 'Reposition overlays', detail: 'Drag the cooldown timer, local time clock, VGP icon, and Keep All toggle anywhere.' },
      { name: 'Hide overlay', detail: 'From the Quick Menu, make the game look stock: hide every PGRazor button, status, and IV text; tap with three fingers to bring them back.' },
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
    caption: 'The PGRazor world map, with saved places, hotspots, routes, nearby layers, copy-location buttons, and Walk here / Teleport controls. Map data © OpenStreetMap contributors.',
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
      'The full-screen Settings home in dark theme, with Search settings at the top and pages grouped under Play, Automation, and App, with a Discord link in the footer.',
    width: 1080,
    height: 1920,
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
