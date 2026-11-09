# MuMu regression coverage

Internal verification checklist, not release notes. Target: `127.0.0.1:16416`.
A native fixture pass does not establish live gameplay completion. A click, ready flag,
or return to the map alone is insufficient evidence of a successful server action.

Paused at the user's requested commit/publish checkpoint on 2026-11-09.
Coverage is incomplete; pending rows below are not passes.

Latest private candidate: `20261109-082841-7ae3c4cb`, based on source `90f633a`
plus the changes on this branch, APK SHA256
`84f7c945858f9efa073d37a49fed0c35c5614e024f1b4d1939e9e13445e87312`.
Installed only on MuMu; cold startup/auth passed in
`.artifacts/device-regression/20261109-163240-868044f4`.
Full `tools/check.py` PASS, including192 Python host tests, Java/native host checks,
Node fixtures and schema/bundle checks: `.artifacts/regression-goal/precommit-check.txt`.
The earlier111 native harnesses passed; detailed baseline receipts are in
`.artifacts/overnight-regression/validation-summary.json`.

| Feature | Required live assertion | Current evidence / next action |
| --- | --- | --- |
| Runtime startup | Same launched process reaches both native-ready events | Passed two cold launches and both overnight cycles |
| Authenticated session | Game identity matches active profile after each launch | Passed both overnight cycles; not forced token expiry |
| Long stability/background interval | Same PID and no native failure throughout observation; authenticate after home idle | Passed two 3.5-hour sampled observations separated by one hour on home; report `.artifacts/device-regression/20261109-053710-42ca2013` |
| Settings persistence/restoration | Change through app setter, read runtime value after restart, restore original and verify | Automated PASS on prior test candidate `20261109-065055-a4d70c97`: `.artifacts/settings-regression/20261109-145434-c0bbbbae`; actual setter, new PID, persistence and restoration; unrelated checked settings unchanged |
| Quick Spin | Eligible stop action produces positive server rewards/inventory changes | PASS on `20261109-070620-16f30bb6`: `.artifacts/regression-goal/quick-spin/report.json`; +3 Poke Balls/+1 Great Ball/+1 Potion screenshot exactly matches fresh inventory delta, same PID. Immediate cooldown retry showed Try again later and no delta (`cooldown.json/png`). Item disposal/map-tap mode restored exactly (`restoration.json`); bag-full branch pending |
| Quick Catch | Matching encounter is caught, inventory contains result, returns to map | Positive PASS on `20261109-071933-cb06d577`: `.artifacts/regression-goal/quick-catch/report.json`; Venonat CP320, level12, IV13/7/9, native caught result, map reward, matching storage/detail. Settings restored (`restoration.json`). Escape/flee branches pending |
| Always Hit / throw quality / curve / AR flag | Real throw observation and corresponding game response | Quick Catch evidence records Always Hit and AR applied, curve true, grade Off, successful catch. Controlled comparisons and other grades pending |
| IV/level/catch chance/auto ball | Display matches encounter facts; selected ball meets configured policy | One-target PASS: `.artifacts/regression-goal/quick-catch/iv-report.json`; Venonat encounter/detail/appraisal agree, auto-ball selected lowest qualifying ball (82.04% vs75% target). Boundary/fallback cases pending |
| Berry selector | Selected berry consumed once, matching encounter effect | Pinap priority1 PASS: `.artifacts/regression-goal/quick-catch/berry-report.json`; one feed request, count197→196, six candy on catch. Other priorities/fallback cases pending |
| Inventory IV/details | Values match appraisal and list/detail identity | One-target PASS: Venonat CP320/level12/64.4%, attack13/defense7/HP9 agree across encounter, storage/detail and native appraisal (`quick-catch/iv-report.json`). Other formats pending |
| Transfer/keep rules | Disposable newly caught target removed only when eligible; protected targets retained | Fixtures passed; explicit disposable targets needed before destructive test |
| Item limits | Only excess configured items removed, no unrelated loss | Fixtures passed; disposable item budget needed |
| VGP and VGP+ | Catch/spin server outcomes and inventory changes; disable and foreground gates | VGP+ spin-only positive PASS on `20261109-073750-26175bb7`: `.artifacts/regression-goal/vgp/report.json`; three native spin successes, positive inventory delta, Pokemon count unchanged262, then OFF and options restored. Existing disposal remained ON; exact per-spin rewards not captured. Catch and foreground-gate cases pending |
| Unlimited incubator | Eligible egg placed once without premium incubator use | Free unlimited incubator occupied by10km egg at6.4km; `.artifacts/regression-goal/incubation/eggs-before.png`. Eligible unassigned eggs exist; positive placement waits for free incubator |
| Buddy feeding/emotion | Berry delta and game hunger/emotion progress, no repeat overfeed | Native lost/ambiguous-result stall reproduced against old core and fixed: 18 scenarios PASS `pgrazor/build/native/harness/20261109-160607-007824`. Recovery requires same buddy observed fed after timeout; never credits uncertain results or retries on timeout alone. Different-buddy uncertainty and very late RPC correlation remain limitations. Live berry-delta scenario pending |
| Gift open/send/stickers | Matching friend action and inventory/result, limits honored | Fixtures passed; suitable friend/gift/sticker prerequisites |
| Field research | Eligible claim/delete/encounter affects correct task only | Fixtures passed; explicitly selected disposable tasks required for deletion |
| Rocket battle/minigame/shields | Actual battle outcome and controlled charge/shield actions | Fixtures passed; live Rocket target pending |
| Gym/raid/Max battles | Controlled battle actions and game outcome, no stalled phase | Fixtures passed; suitable target and resource budget pending |
| Gym deploy/feed/spin | Team capacity/defender eligibility respected, confirmed server result | Fixtures passed; suitable gym scenario pending |
| Routes | Discovery/start/progress/reward; pause/cancel prevent replay | Fixtures passed; available route pending |
| Shundo Sniper | Eligible target travel, encounter selection and outcome; restore state | Live controlled destination/account travel scenario pending |
| Cutscene skips | Supported scene ends in correct playable state without duplicate reward | Fixtures passed where implemented; scene-specific live triggers pending |
| Batch evolution | Preview excludes protected entries; confirmed evolution/inventory | Fixtures passed; disposable tagged Pokemon/candy prerequisite |
| Map/joystick/location | Correct direction and persisted stationary location; stop on background | Background stop/no automatic resume PASS `autowalk/background/report.json`. Four manual directions PASS on081017: `joystick/report.json`; opposite vectors reverse, axes perpendicular, each release stops, same process. All properties and original location/speed restored. Camera rotation comparison pending |
| Autowalk/GPX/favorites | Ordered route import/export and traversal; cancel stops movement | GPX picker import/save/load PASS `autowalk/save-report.json`; 5 coordinates/order preserved. Actual traversal PASS `autowalk/traversal-2/traversal-report.json`: ordered visits at0/33/67/102/136s, returned to origin, stationary end. Background stop/no resume PASS `autowalk/background/report.json`. Explicit stop PASS `autowalk/explicit-stop-report.json`; SAF export exactly matches all5 original coordinates/order (`autowalk/export-report.json`). Favorite save/list/selection/delete PASS `autowalk/favorites-report.json`; original empty library, location/speed restored and owned GPX fixture removed via UI. Favorite cold restart pending. Initial traversal setup failure retained |
| Map layers/POI/tiles | Loaded objects match game facts; map gestures and details remain usable | Live200 loaded POIs displayed with all3 layers enabled; raid detail automatically became gym detail after expiry, removed boss/timer and later updated defender count. Screenshots `autowalk/map-layers.png`, `raid-detail.png`, `raid-expired-detail.png`, `current-map.png`. Raw `Raid _5` shortlist label fixed; live `Raid 5`/`Raid 3` PASS on082841 (`autowalk/raid-label-fixed.png`). All3 original layer settings OFF restored/read back (`checkpoint.json`). Independent game-object comparison pending |
| LivePokeMap/feeds | Authenticated feed and filters match server results | Subscription/data prerequisite unverified |
| Accounts | Switch both directions, verify identity and isolated settings/location | Saved test profiles/session prerequisites unverified; never assume startup alone proves switching |
| Settings backup/restore | Export/modify/import restores included fields and excludes credentials; VGP off | Real Android file-picker round trip PASS on `20261109-074137-29481c6b`: `.artifacts/regression-goal/backup-ui/restoration.json`; exported87 settings, changed joystick, imported, re-exported structurally identical settings/preferences/speed. Runtime and entire properties restored, VGP OFF. Earlier in-process mutation/restoration also passed. Always Hit whitelist omission fixed. Backup screen heading overlap fixed and live PASS on081017 (`backup-ui/fixed-heading.png`) |
| UI/theme/scale/menu/hide overlays | Render and interact across sizes; restore hidden overlays and original settings | PASS subset on `20261109-074137-29481c6b`: `.artifacts/regression-goal/ui-regression/report.json`; light-theme status icons fixed with API30+ controller and restored on close; overlay/menu25/50/75/100% usable; Buddy Emotion now scales. Original dark/100% restored. Actual Android touch injection PASS `ui-regression/hide-gesture-report.json`: one/two fingers retain hidden state, three restore; original location/speed restored after tap-to-walk passthrough. Rotation and active catch overlays at small scales pending |
| Battery/performance | Settings affect FPS/rendering without losing gameplay/restore | Map-scene PASS on `20261109-081017-22650c65`, `.artifacts/regression-goal/battery/report.json`: SurfaceFlinger127-frame samples20.05FPS→30FPS cap-off→20FPS restored; actual native render-scale getter0.5→1.0→0.5, quality getters restore game shadows/fog/HDR when off. Same process and all properties restored. Encounter plants, sniper dark mode and physical battery/thermal benefit not measured |

Update rows only when the matching assertion is evidenced. Keep missing prerequisites
and unimplemented adapters distinct from feature failures. Record candidate identity,
original settings, restoration result, screenshots and sanitized outcomes per run.

Inventory reads use unique native scan tokens; stale/foreign/expired responses are
rejected and completed counts frozen. Normal UI refreshes still deduplicate. The
native regression scan disables recycling even when disposal is enabled mid-read;
fixture PASS `pgrazor/build/native/harness/20261109-151812-920068`. Live PASS with
original disposal ON on073750: `.artifacts/regression-goal/inventory-disposal-on/report.json`;
counts, item rules/limits and settings unchanged. These counts do not prove catches.

The outcome parser handles compiled `quick-close`/`kind=vgp-result` envelopes;
9 targeted tests passed after the missing-event bug was reproduced live.
Real backup round trip/restoration on071933:
`.artifacts/feature-regression/20261109-152353-9b2a66c6`; joystick cold-restart
persistence/restoration: `.artifacts/settings-regression/20261109-152402-56931642`.

Checkpoint restoration: `.artifacts/regression-goal/checkpoint.json`. All original
properties, location/speed and map-layer settings restored; no backup recovery
pending; owned favorite and saved GPX fixture removed via UI. MuMu parked at Home
and stationary. Installed082841 and tested rollback081017 are retained locally.
Private screenshots, account data and test APKs are not published with the branch.

One Claude buddy implementation worker timed out without edits; the coordinator
implemented and tested the fix. Successful read-only worker reports were reviewed;
the claim that different-buddy pending blocking was newly introduced was rejected
after checking the old implementation. This limitation remains documented above.

Initial packaging failed Windows Java memory allocation; process-local
`JAVA_TOOL_OPTIONS=-Xms64m -Xmx2g` allowed packaging to complete. Diagnostics retained.
Packaging cleanup `pgrazor/build/cleanup/20261109-162946-1020841.json` reclaimed0GiB;
the earlier verified cleanup reclaimed1.385GiB of obsolete APK replicas.
