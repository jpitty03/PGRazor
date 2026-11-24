# Hundo feed collector

Publishes the existing PGRazor `v:1` JSON format. Switching providers or map coverage requires only a server update; the APK and public feed URL stay the same.

## Server configuration

Install `hundo_feed.py` at `/opt/hundo-feed/hundo_feed.py`. Configure `/etc/hundo-feed.env` using the example, preserving `HUNDO_OUT`. Use `HUNDO_PROVIDER=pogocoordinates` and set `POGO_TOKEN` to the bearer value without the prefix. Keep the env mode 600. Restart with `sudo systemctl restart hundo-feed`; inspect `sudo journalctl -u hundo-feed -n 30 --no-pager`. The script itself does not load the env file; systemd does.

## Merged sources

`HUNDO_SOURCES` (for example `pogocoordinates,citymaps`) runs several sources in one process. Each has its own interval and back-off, so a long provider cooldown (PoGoCoordinates once returned 45,360 s) never stalls the others. After every round the collector publishes the merged list: `hundos` holds one row per encounter ID (earlier sources win a duplicate), and `spots` holds spawns from sources without encounter IDs. A spot is dropped when a listed hundo, or an earlier spot, is the same spawn: same species, within 50 m, despawning within three minutes. `sources` reports each source's active and unique counts and status. PoGoCoordinates spends no reveal on an encounter another source already lists with exact coordinates.

`citymaps` reads the five city PokeMaps' `query2.php` 100% IV lists (one request per site per `CITYMAP_INTERVAL`, default 60 s). They give exact coordinates but no encounter ID. Spot rows use the hundo columns with a key in place of the ID: `s` plus 16 hex digits of SHA-1 over `dex:lat:lon:despawn-minute` (coordinates to five decimals). PGRazor builds with spot support find the spawn by species within 50 m of the point and check that it is 15/15/15; older builds ignore `spots`. moonani.com's list is read by the app itself, because its Cloudflare blocks server addresses.

## Production sniper feed

Use exact mode for the sniper. The approximate-marker rollout caused repeated misses on the Pixel: a sampled target had over 26 minutes left but its marker was 329.5 metres from its revealed location. Production was restored to exact mode, one new reveal per 300-second poll. This conservative baseline yields a small feed. VIP account access has since been confirmed; its numeric reveal quota is not documented by the supplied membership table, so 1,500 exact-coordinate targets remain unverified.

## VIP exact-coordinate collection

The supplied VIP membership table lists 500 map results per lookup and a High coordinate-reveal allowance without a numeric quota. The existing account token reports tier vip. Set POGO_LOOKUP_RESULTS=500 to request that documented result allowance in exact mode. POGO_REVEAL_PAUSE_SECONDS controls spacing between reveal requests (1..30 seconds); POGO_REVEALS_PER_POLL controls the batch budget. Changes to these settings do not override provider caps or cooldowns. The collector skips new reveal requests for spawns with less than 150 seconds remaining and prefers longer-lived spawns. Stage higher reveal budgets with live checks; the old logged-in-tier 1-per-300-second setting is a fallback, not a VIP quota. The VIP rollout successfully tested ten reveals at two-second spacing, then passed two batches of 25 reveals at two-second spacing. A 50-reveal trial at one-second spacing hit HTTP429 after28successful reveals. The deployed setting was reduced to25new exact reveals per120-second cycle, spaced two seconds apart, with500results per lookup; verification resumes only after the reported cooldown. These are local collector settings, not documented provider quotas; 429 cooldowns still apply and long-term sustainable throughput is not established.

## Diagnostic map coverage

`POGO_COORDINATE_MODE=map` collects the API's approximate marker coordinates (`apx_lat`, `apx_long`) into the separate `map_candidates` diagnostic list. They are excluded from `hundos`, the list consumed by the sniper, even in map mode. Only previously cached exact positions may enter that list in map mode. The site explicitly calls these positions approximate. Live comparisons measured offsets of approximately 149 to 475 metres from exact locations. Their accuracy for successful in-game encounters is not guaranteed. Existing PGRazor builds ignore diagnostic map_candidates. The original rollout incorrectly placed approximate markers in hundos; a regression test now prevents this.

`POGO_TILE_DEGREES=30` queries a fixed grid of 72 regions across latitude -85..85 and longitude -180..180. `POGO_REFINE_DENSE=1` subdivides 14 regions across North America, Europe, Brazil, East Asia and southeast Australia into 15-degree cells, giving 114 total regions with the same complete geographic bounds. Requests run sequentially, with five seconds between regions, in batches of 18. The next region is saved with the feed so scans resume after errors and restarts. Each region asks for 100 results; a live request for 1,500 still returned only 100. Each batch publishes at a minimum interval of 120 seconds. `POGO_PRIORITIZE_ACTIVE=1` spends three selection slots revisiting regions containing active cached spawns and every fourth slot on worldwide discovery. Separate saved cursors rotate both groups; a region is queried at most once per batch. This refreshes productive areas more often instead of repeatedly polling empty ocean regions. A full discovery rotation can take roughly 51 minutes when the priority queue is busy; known populated regions refresh more frequently, depending on their count. With priority disabled the refined grid rotates in about 13 minutes. Slow requests or rate limits make it take longer. Custom bounds use the same grid within those bounds. Supported tile sizes are 15, 30, 45 and 60 degrees. These are ordinary geographic queries; authentication, per-request caps and rate limits continue to apply. Counts depend on live spawn availability and are not guaranteed to reach any fixed target.

The collector retains valid unexpired encounters between scans, deduplicates signed/unsigned encounter IDs, checks 100% IV metadata, and excludes spawns with less than one minute remaining when publishing. It makes no reveal requests in map mode. Previously cached exact coordinates are preserved until expiry; other rows use approximate coordinates only in map_candidates. Named forms such as Alola no longer cause otherwise valid entries to be discarded. Their names are preserved in `form_names`; the compact row's numeric form is zero when no numeric form ID is supplied.

Output labels the sniper hundos list with `coordinate_precision=exact`. Diagnostic map mode also adds `map_candidates`, `map_coordinate_precision` (`approximate` or `mixed`), `approximate_encounters` (IDs in the diagnostic list), `form_names`, and `lookup_regions`. No bearer tokens or reveal codes are published. Accuracy metadata persists across restarts; switching back to exact mode excludes approximate cache entries. An error stops the scan, publishes any still-valid partial progress, and waits before retrying. HTTP 429 honors the larger of Retry-After and detail.retry_after from the response body, defaulting to five minutes if absent.

## Exact coordinates

`POGO_COORDINATE_MODE=exact` uses eight large worldwide lookup regions and `/reveal/{code}` for exact coordinates, with `POGO_REVEALS_PER_POLL=1` and a conservative 300-second interval. This is a conservative local budget, not a confirmed provider allowance. The former setting of two reveals per 90 seconds encountered a 429 with a 2,214-second cooldown during deployment. That pre-VIP allowance could not supply broad exact coverage; the upgraded account is being validated separately. Reveal calls may consume account allowance. Cache persists until expiry and survives restart. A single worldwide request exceeds the provider's observed 12,500 km geographic limit.

For either mode, a revoked/expired token requires updating the env and restarting; no automatic Discord login refresh is available. A one-poll check uses `python3 hundo_feed.py --once` with the environment loaded and a separate `HUNDO_OUT`. Incomplete polls exit nonzero. Do not run two collectors against the production output.

## Legacy provider

`HUNDO_PROVIDER=livepokemap` retains the original collector with `LPM_USERNAME`, `LPM_PASSWORD`, and `HUNDO_STATE` for its cached login token. Restoring this configuration does not establish that the provider is healthy; the former production account returned HTTP 403.

## Verification

Run `python3 -B -m unittest discover -s tools/hundo-feed -p 'test_*.py'`. Tests use fixtures without network requests or credentials. Live deployment evidence and cleanup receipts belong in internal artifacts, not public release notes.

