// Real MuMu captures; original evidence and crop receipts are retained locally.
const shots = {
  "vgp-plus": {"id": "vgp-plus", "src": "images/guides/vgp-plus.webp", "width": 1040, "height": 1340, "title": "VGP+ and gym disc settings", "caption": "VGP configuration showing nearby targets, estimated cooldown control, ball and throw choices, and Auto-spin Gym discs. Start or stop through the VGP overlay."},
  "buddy-emotion": {"id": "buddy-emotion", "src": "images/guides/buddy-emotion.webp", "width": 1040, "height": 740, "title": "Buddy care settings", "caption": "Buddy Emotion Engine configuration with berry reserves, optional gift collection and EP overlay. The status reflects this capture, not a guaranteed excitement time."},
  "buddy-priority": {"id": "buddy-priority", "src": "images/guides/buddy-priority.webp", "width": 959, "height": 615, "title": "Choose a berry order", "caption": "The numbered berry selections determine feeding order: Nanab first, then Razz, then Pinap in this example. Premium berries are unchecked. Save applies the choice; Cancel leaves it unchanged."},
  "batch-evolution": {"id": "batch-evolution", "src": "images/guides/batch-evolution.webp", "width": 1015, "height": 872, "title": "Preview a tagged batch", "caption": "Batch evolution before preview: enter the Evolve tag and optionally enable tag removal. Start remains disabled until an eligible batch is previewed. No evolution was started for this screenshot."},
  "battles": {"id": "battles", "src": "images/guides/battles.webp", "width": 1040, "height": 895, "title": "Battle automation choices", "caption": "Separate quick attack, charged attack and dodge controls for Raids, Gyms and Dynamax. The Gym quick or charged attack setting also enables next-opponent continuation."},
  "gym-slotter": {"id": "gym-slotter", "src": "images/guides/gym-slotter.webp", "width": 1015, "height": 905, "title": "Gym deployment and feeding settings", "caption": "Deployment and feeding choices are selected, but the Run switch is off. The optional fallback and zero berry reserves shown are example preferences, not the defaults."},
  "event-boxes": {"id": "event-boxes", "src": "images/guides/event-boxes.webp", "width": 959, "height": 402, "title": "Halloween box collection", "caption": "The Halloween event setting opens visible boxes and skips Pokemon encounter screens without catching. It does not select only candy boxes."},
  "hotspots": {"id": "hotspots", "src": "images/guides/hotspots.webp", "width": 1080, "height": 1860, "title": "Choose a hotspot", "caption": "The curated hotspot list in the Quick Menu. Listed UTC offsets are reference values and may vary seasonally."},
  "hotspot-actions": {"id": "hotspot-actions", "src": "images/guides/hotspot-actions.webp", "width": 808, "height": 286, "title": "Walk or teleport to a hotspot", "caption": "Selecting Pier 39 from Quick Menu Hotspots offers Walk and Teleport. Teleport opens a separate cooldown confirmation."},
  "teleport-cooldown": {"id": "teleport-cooldown", "src": "images/guides/teleport-cooldown.webp", "width": 808, "height": 306, "title": "Check the destination cooldown", "caption": "Cooldown confirmation for Pier 39. Zero is the estimate for this capture; your recorded actions and elapsed time determine your estimate. The teleport was canceled."},
  "world-map": {"id": "world-map", "src": "images/guides/world-map.webp", "width": 1080, "height": 1860, "title": "Select a destination on the map", "caption": "Pier 39 selected as the red destination marker, with search, hotspot and map-layer controls. Selecting a destination does not move the character; Walk here or Teleport is a separate action. Map data: OpenStreetMap contributors."},
};
export const guideScreenshots = {
  "vgp-plus": [shots["vgp-plus"]],
  "virtual-go-plus": [shots["vgp-plus"]],
  "buddy-emotion": [shots["buddy-emotion"], shots["buddy-priority"]],
  "batch-evolution": [shots["batch-evolution"]],
  "battles": [shots["battles"]],
  "gym-slotter": [shots["gym-slotter"]],
  "event-boxes": [shots["event-boxes"]],
  "hotspots-favorites": [shots["hotspots"], shots["hotspot-actions"]],
  "cooldown": [shots["teleport-cooldown"]],
  "world-map": [shots["world-map"]],
};
