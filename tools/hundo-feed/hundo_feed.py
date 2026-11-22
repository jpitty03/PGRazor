#!/usr/bin/env python3
"""PGRazor hundo feed: polls a configured provider's 100% IV list and publishes it as a
static JSON file, so the app can read it without each user signing in.

Runs on a server (systemd: hundo-feed.service). Standard library only. Configuration comes from the
environment (see hundo-feed.env.example):

  LPM_USERNAME, LPM_PASSWORD   the LivePokeMap account
  HUNDO_PROVIDER              livepokemap (default) or pogocoordinates
  POGO_TOKEN                  PoGoCoordinates bearer token (no Bearer prefix)
  POGO_COORDINATE_MODE        exact (default) or map (approximate coverage, no reveals)
  POGO_TILE_DEGREES           map mode grid size: 15, 30 (default), 45, 60
  POGO_BOUNDS                 south,north,west,east (default worldwide, split into eight regions)
  POGO_REVEALS_PER_POLL        exact-coordinate reveal budget (default 1, shared across regions)
  POGO_LOOKUP_RESULTS          results requested per region (default 100; VIP allows 500)
  POGO_REVEAL_PAUSE_SECONDS    spacing between exact-coordinate requests (default 1)
  HUNDO_OUT                    where to write the file, e.g. /var/www/example.com/pgrazor/hundos.json
  HUNDO_INTERVAL               seconds between polls (default 30 for LPM / 300 for PoGo, at least 15)
  HUNDO_STATE                  where the session token is kept (default /var/lib/hundo-feed/token)

The file is a compact list, written atomically (temp file + rename):

  {"v": 1, "fetched_ms": <ms>, "interval_s": 30,
   "hundos": [[encounter_id, dex, form, lat, lon, despawn_ms, cp, level], ...]}

encounter_id is a decimal string (64-bit). Only 100% IV Pokemon (15/15/15) with at least a minute left
are kept; everything else in LivePokeMap's response (forts, stations, other fields) is dropped.
"""
import json
import argparse
import math
import os
import re
import sys
import time
import urllib.error
import urllib.request
import urllib.parse

BASE = "https://play.livepokemap.com"
POGO_API = "https://coordinates-api.pokemongopro.com"
USER_AGENT = "PGRazor-hundo-feed/1.0 (+https://github.com/jpitty03/PGRazor)"
TIMEOUT_S = 20
WORLD = (-85.0, 85.0, -180.0, 180.0)  # min_lat, max_lat, min_lon, max_lon
MAX_TILE_LON = 90.0  # widest tile below the whole map: LivePokeMap sends nothing for a 180 degree one
MIN_TILE_DEG = 1.0  # a tile side this short is not halved again
TILE_PAUSE_S = 1.0  # pause between tile requests
RESET_POLLS = 40  # start again from one whole-map request this often, so tiles do not stay small for ever


def log(message):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), message, flush=True)


class FeedError(Exception):
    def __init__(self, message, status=0, retry_after=0):
        super().__init__(message)
        self.status = status
        self.retry_after = retry_after


def request_json(base, path, body=None, token=None):
    request = urllib.request.Request(base + path, data=json.dumps(body).encode("utf-8") if body is not None else None,
                                     method="POST" if body is not None else "GET")
    request.add_header("Content-Type", "application/json; charset=utf-8")
    request.add_header("Accept", "application/json")
    request.add_header("User-Agent", USER_AGENT)
    if token:
        request.add_header("Authorization", "Bearer " + token)
    if base == POGO_API:
        request.add_header("Referer", "https://pogocoordinates.com/")
        request.add_header("Origin", "https://pogocoordinates.com")
    safe_path = "/reveal/[redacted]" if path.startswith("/reveal/") else path
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_S) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        retry = error.headers.get("Retry-After") if error.headers else None
        try:
            retry_s = max(0, int(retry)) if retry else 0
        except ValueError:
            retry_s = 0
        # This provider also returns cooldown seconds in detail.retry_after.
        try:
            detail = json.loads(error.read().decode('utf-8')).get('detail', {})
            if isinstance(detail, dict):
                seconds = float(detail.get('retry_after') or 0)
                if math.isfinite(seconds):
                    retry_s = max(retry_s, math.ceil(seconds))
        except (ValueError, TypeError, AttributeError, OSError):
            pass
        raise FeedError("HTTP %d from %s" % (error.code, safe_path), error.code, retry_s)
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise FeedError("network error on %s (%s)" % (safe_path, type(error).__name__))
    except json.JSONDecodeError:
        raise FeedError("non-JSON reply from %s" % safe_path)


def post(path, body, token=None):
    return request_json(BASE, path, body, token)


def lookup_body(bounds=WORLD, num_results=100):
    return {
        "mode": "pokemon", "s_lat": bounds[0], "n_lat": bounds[1], "w_lng": bounds[2], "e_lng": bounds[3],
        "num_results": num_results,
        "filters": {
            "pokemon_ids": [], "species_forms": [], "blacklist_pokemon_ids": [], "blacklist_species_forms": [],
            "iv_min": 100, "iv_max": None, "cp_min": None, "cp_max": None, "level_min": None, "level_max": None,
            "gender": None, "sizes": [], "boosted": None, "sort": "IV", "pvp_mode": False,
            "pvp_league": "great", "pvp_top_rank": None, "pvp_include_pre_evolutions": True, "pvp_min_cp": None,
            "shiny_only": False, "shiny_account": None, "background_only": False, "always_show": [],
        },
    }


def lookup_tiles(bounds):
    """Split world bounds for the API's measured 12,500 km geographic limit, not account quotas."""
    if bounds != WORLD:
        return [bounds]
    return [(south, north, west, west + 90) for south, north in ((-85.0, 0.0), (0.0, 85.0))
            for west in (-180.0, -90.0, 0.0, 90.0)]


def pogo_row(o, now_ms):
    """Validate metadata before spending a reveal request. Never use approximate map coordinates."""
    if not isinstance(o, dict):
        return None
    try:
        raw_iv = o.get("raw_iv")
        percent = o.get("iv_percent")
        if raw_iv is not None and raw_iv != "15/15/15":
            return None
        if percent is not None and float(percent) != 100:
            return None
        if raw_iv != "15/15/15" and percent is None:
            return None
        encounter = o.get("encounter_id")
        if isinstance(encounter, bool) or not isinstance(encounter, (str, int)) or not re.fullmatch(r"-?[0-9]{1,20}", str(encounter)):
            return None
        encounter = int(encounter)
        if not -(1 << 63) <= encounter < (1 << 64) or encounter == 0:
            return None
        encounter = str(encounter % (1 << 64))
        # The map API uses names such as "Alola", unlike LivePokeMap's numeric forms.
        # Named forms must not cause otherwise valid encounters to be discarded.
        raw_form = o.get("form")
        dex = int(o["pokemon_id"])
        form = int(raw_form or 0) if not isinstance(raw_form, str) or raw_form.isdigit() else 0
        expiry = int(o["despawn_at_ms"])
        cp, level = int(o.get("cp") or 0), float(o.get("level") or 0)
        if dex <= 0 or form < 0 or cp < 0 or not math.isfinite(level) or not 0 <= level <= 55 or expiry < now_ms + 60_000:
            return None
        return [encounter, dex, form, None, None, expiry, cp, level]
    except (KeyError, ValueError, TypeError, OverflowError):
        return None


def revealed_coords(response):
    if not isinstance(response, dict):
        raise FeedError("unexpected reveal response")
    try:
        if response.get("lat") is not None and response.get("lng") is not None:
            lat, lon = float(response["lat"]), float(response["lng"])
        else:
            lat, lon = map(float, response["coords"].split(","))
        if not (-90 <= lat <= 90 and -180 <= lon <= 180):
            raise ValueError()
        return round(lat, 6), round(lon, 6)
    except (KeyError, ValueError, TypeError, AttributeError):
        raise FeedError("reveal response has no valid exact coordinates")


class PogoLookup:
    def __init__(self, token, out, reveal_budget=1, bounds=WORLD, lookup_results=100, reveal_pause=1):
        self.token, self.bounds, self.reveal_budget = token, bounds, reveal_budget
        self.lookup_results, self.reveal_pause = lookup_results, reveal_pause
        self.cache = {}
        # Reuse exact coordinates across restarts; never persist bearer/reveal tokens.
        try:
            with open(out, encoding="utf-8") as source:
                previous = json.load(source)
            if previous.get("source") == "pogocoordinates" and previous.get("bounds") == list(bounds):
                now_ms = int(time.time() * 1000)
                for row in previous.get("hundos", []):
                    if row and row[0] in previous.get("approximate_encounters", []):
                        continue  # Exact-only mode must not restore approximate positions.
                    if (isinstance(row, list) and len(row) == 8 and isinstance(row[0], str) and row[0].isdigit()
                            and 0 < int(row[0]) < (1 << 64) and row[1] > 0 and row[5] >= now_ms + 60_000
                            and -90 <= row[3] <= 90 and -180 <= row[4] <= 180):
                        self.cache[row[0]] = row
        except (OSError, ValueError, TypeError, AttributeError):
            self.cache = {}

    def poll(self):
        results = []
        tiles = lookup_tiles(self.bounds)
        for index, tile in enumerate(tiles):
            if index:
                time.sleep(1)
            response = request_json(POGO_API, "/map/lookup", lookup_body(tile, self.lookup_results), self.token)
            if not isinstance(response, dict) or response.get("mode") != "pokemon" or not isinstance(response.get("results"), list):
                raise FeedError("unexpected map lookup response; keeping previous published feed")
            results.extend(response["results"])
        now_ms = int(time.time() * 1000)
        self.cache = {key: row for key, row in self.cache.items() if row[5] >= now_ms + 60_000}
        pending, seen = [], set()
        for item in results:
            row = pogo_row(item, now_ms)
            if row is None or row[0] in seen:
                continue
            seen.add(row[0])
            if row[0] in self.cache:
                row[3:5] = self.cache[row[0]][3:5]
                self.cache[row[0]] = row
            elif isinstance(item.get("reveal_code"), str) and item["reveal_code"]:
                pending.append((row, item["reveal_code"]))
        # Prefer spawns with enough life remaining to be useful after the reveal queue.
        pending.sort(key=lambda pair: pair[0][5], reverse=True)
        error, revealed = None, 0
        for row, code in pending[:self.reveal_budget]:
            if revealed:
                time.sleep(self.reveal_pause)
            if row[5] < int(time.time() * 1000) + 150_000:
                continue  # Do not spend reveal quota on a spawn the sniper will skip.
            try:
                coords = request_json(POGO_API, "/reveal/" + urllib.parse.quote(code, safe=""), token=self.token)
                row[3:5] = revealed_coords(coords)
                self.cache[row[0]] = row
                revealed += 1
            except FeedError as exc:
                error = exc
                break  # No retry storms or bypassing account limits.
        now_ms = int(time.time() * 1000)
        rows = [row for row in self.cache.values() if row[5] >= now_ms + 60_000]
        log("%d lookup regions returned %d records; revealed %d; %d exact active hundos; result caps may limit coverage" %
            (len(tiles), len(results), revealed, len(rows)))
        return rows, error


class PogoMapCoverage:
    """Broad map coverage using explicitly labelled approximate positions; no reveal calls."""
    def __init__(self, token, out, bounds=WORLD, tile_degrees=30, pause=5, regions_per_poll=18, refine_dense=False, prioritize_active=False):
        self.token, self.bounds, self.pause = token, bounds, pause
        self.regions_per_poll = regions_per_poll
        self.cursor, self.tile_degrees = 0, tile_degrees
        self.refine_dense = refine_dense
        self.prioritize_active = prioritize_active
        self.priority_cursor, self.selection_step = 0, 0
        self.cache, self.exact, self.forms = {}, set(), {}
        self.comparison_meters = []
        south, north, west, east = bounds
        self.tiles = []
        lat = south
        while lat < north:
            lon = west
            while lon < east:
                tile = (lat, min(lat + tile_degrees, north), lon, min(lon + tile_degrees, east))
                # Finer geography across North America and Europe; the rest of the
                # world remains in the same scan rotation at the same request pace.
                dense = ((lat == 35 and lon in (-150, -120, -90, -30, 0, 30, 120))
                         or (lat == 5 and lon in (-120, -90, 90, 120))
                         or (lat == -25 and lon in (-60, -30)) or (lat == -55 and lon == 150))
                if refine_dense and bounds == WORLD and tile_degrees == 30 and dense:
                    self.tiles.extend((y, y + 15, x, x + 15) for y in (lat, lat+15) for x in (lon, lon+15))
                else:
                    self.tiles.append(tile)
                lon += tile_degrees
            lat += tile_degrees
        try:
            with open(out, encoding="utf-8") as source:
                previous = json.load(source)
            if previous.get("source") == "pogocoordinates" and previous.get("bounds") == list(bounds):
                if (previous.get('tile_degrees') == tile_degrees and previous.get('refine_dense', False) == refine_dense
                        and previous.get('lookup_regions') == len(self.tiles)):
                    self.cursor = int(previous.get('next_region', 0)) % len(self.tiles)
                    self.priority_cursor = int(previous.get('priority_cursor', 0))
                    self.selection_step = int(previous.get('selection_step', 0))
                approximate = set(previous.get("approximate_encounters", []))
                now = int(time.time() * 1000)
                for row in previous.get("map_candidates", previous.get("hundos", [])):
                    if (isinstance(row, list) and len(row) == 8 and isinstance(row[0], str)
                            and row[0].isdigit() and 0 < int(row[0]) < (1 << 64)
                            and row[1] > 0 and row[5] >= now + 60_000
                            and -90 <= row[3] <= 90 and -180 <= row[4] <= 180):
                        self.cache[row[0]] = row
                        if row[0] not in approximate:
                            self.exact.add(row[0])
                self.forms = previous.get("form_names", {})
        except (OSError, ValueError, TypeError, AttributeError):
            self.cache, self.exact, self.forms = {}, set(), {}

    def poll(self):
        error, received, completed = None, 0, 0
        self.comparison_meters = []
        # Three slots revisit known populated regions; every fourth explores the
        # full world grid. Separate persistent cursors keep both rotations fair.
        now = int(time.time() * 1000)
        active = [row for row in self.cache.values() if row[5] >= now + 60_000]
        priority = [i for i,(s,n,w,e) in enumerate(self.tiles)
                    if any(s <= row[3] < n and w <= row[4] < e for row in active)] if self.prioritize_active else []
        visited = set()
        for index in range(min(self.regions_per_poll, len(self.tiles))):
            cursor, hot, step = self.cursor, self.priority_cursor, self.selection_step
            while True:
                if priority and step % 4 != 3:
                    chosen = priority[hot % len(priority)]
                    hot += 1
                else:
                    chosen = cursor
                    cursor = (cursor + 1) % len(self.tiles)
                step += 1
                if chosen not in visited:
                    break
            tile = self.tiles[chosen]
            if index:
                time.sleep(self.pause)
            try:
                response = request_json(POGO_API, "/map/lookup", lookup_body(tile), self.token)
                if not isinstance(response, dict) or response.get("mode") != "pokemon" or not isinstance(response.get("results"), list):
                    raise FeedError("unexpected map lookup response")
            except FeedError as exc:
                error = exc
                break  # Honor rate limits; retain useful progress from completed regions.
            completed += 1
            visited.add(chosen)
            self.cursor, self.priority_cursor, self.selection_step = cursor, hot, step
            received += len(response['results'])
            now = int(time.time() * 1000)
            for item in response['results']:
                row = pogo_row(item, now)
                if row is None:
                    continue
                try:
                    lat, lon = float(item['apx_lat']), float(item['apx_long'])
                    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
                        continue
                except (KeyError, ValueError, TypeError, OverflowError):
                    continue
                if row[0] in self.exact and row[0] in self.cache:
                    row[3:5] = self.cache[row[0]][3:5]
                    a, b = math.radians(lat), math.radians(row[3])
                    dl = math.radians(lon - row[4])
                    h = math.sin((a-b)/2)**2 + math.cos(a)*math.cos(b)*math.sin(dl/2)**2
                    self.comparison_meters.append(round(12_742_000 * math.asin(min(1, math.sqrt(h))), 1))
                else:
                    row[3:5] = [round(lat, 6), round(lon, 6)]
                self.cache[row[0]] = row
                if isinstance(item.get('form'), str):
                    self.forms[row[0]] = item['form']
            if completed % 12 == 0:
                log("map coverage: %d/%d regions, %d cached encounters" % (completed, len(self.tiles), len(self.cache)))
        now = int(time.time() * 1000)
        self.cache = {key: row for key, row in self.cache.items() if row[5] >= now + 60_000}
        self.exact.intersection_update(self.cache)
        self.forms = {key: value for key, value in self.forms.items() if key in self.cache}
        log("map coverage: %d/%d regions, %d records, %d unique active hundos (%d exact, %d approximate)" %
            (completed, len(self.tiles), received, len(self.cache), len(self.exact), len(self.cache)-len(self.exact)))
        if self.comparison_meters:
            log("approximate coordinate offsets: %d comparisons, %.1f..%.1f meters" %
                (len(self.comparison_meters), min(self.comparison_meters), max(self.comparison_meters)))
        return list(self.cache.values()), error

    def metadata(self):
        return {"coordinate_precision": "mixed" if self.exact else "approximate",
                "approximate_encounters": sorted(set(self.cache) - self.exact),
                "form_names": self.forms, "lookup_regions": len(self.tiles),
                "tile_degrees": self.tile_degrees, "next_region": self.cursor, "refine_dense": self.refine_dense,
                "prioritize_active": self.prioritize_active, "priority_cursor": self.priority_cursor,
                "selection_step": self.selection_step}


def login(username, password):
    response = post("/api/auth/login", {"username": username, "password": password})
    data = response.get("data") if isinstance(response.get("data"), dict) else {}
    token = (data.get("token") or response.get("token") or "").strip()
    if not token:
        raise FeedError("sign-in did not return a session")
    return token


def viewport(token, tile=WORLD):
    # A wide span: LivePokeMap then sends only 100% IV Pokemon (its "perfect" span tier).
    body = {
        "min_lat": tile[0], "max_lat": tile[1], "min_lon": tile[2], "max_lon": tile[3], "zoom": 15,
        "display_filters": {
            "show_all": True, "min_iv": 100, "max_iv": 100, "min_level": 0, "max_level": 50,
            "always_show_perfect": False, "always_show_zero": False, "always_show_rare": False,
            "always_show_size": False,
        },
    }
    return post("/api/map/viewport", body, token)


def data_of(response):
    return response.get("data") if isinstance(response.get("data"), dict) else response  # {success, data: {...}}


def parts(tile):
    """The smaller tiles that replace a tile: the whole map becomes bands of MAX_TILE_LON, anything else is
    cut in half across its longer side. None when the tile is already at the smallest size."""
    min_lat, max_lat, min_lon, max_lon = tile
    if max_lon - min_lon > MAX_TILE_LON:
        count = int(-(-(max_lon - min_lon) // MAX_TILE_LON))
        step = (max_lon - min_lon) / count
        return tuple((min_lat, max_lat, min_lon + step * i, min_lon + step * (i + 1)) for i in range(count))
    if max_lon - min_lon >= max_lat - min_lat:
        if max_lon - min_lon <= MIN_TILE_DEG:
            return None
        middle = (min_lon + max_lon) / 2.0
        return (min_lat, max_lat, min_lon, middle), (min_lat, max_lat, middle, max_lon)
    if max_lat - min_lat <= MIN_TILE_DEG:
        return None
    middle = (min_lat + max_lat) / 2.0
    return (min_lat, middle, min_lon, max_lon), (middle, max_lat, min_lon, max_lon)


def world(token, tiles):
    """(responses, tiles): viewport responses that together cover the whole map, and the tiles to ask next time.

    LivePokeMap answers with limit_reached and no Pokemon when one request matches too many (about 10,000).
    A tile that hits the limit is replaced by smaller ones; the tiles that worked are remembered, so the next
    poll does not repeat the requests that failed. An empty whole-map answer counts as over the limit too.
    """
    pending = list(reversed(tiles))
    responses, kept = [], []
    first = True
    while pending:
        tile = pending.pop()
        if not first:
            time.sleep(TILE_PAUSE_S)
        first = False
        response = viewport(token, tile)
        root = data_of(response)
        over = root.get("limit_reached") or (tile[3] - tile[2] > MAX_TILE_LON and not root.get("pokemon"))
        if not over:
            responses.append(response)
            kept.append(tile)
            continue
        smaller = parts(tile)
        if smaller:
            pending += reversed(smaller)
        else:
            kept.append(tile)  # asked again next time
            log("lat %.2f..%.2f lon %.2f..%.2f is still over LivePokeMap's limit; left out of this poll" % tile)
    return responses, kept


def compact(response, now_ms):
    """The 100% IV Pokemon of a viewport response as [encounter_id, dex, form, lat, lon, despawn_ms, cp, level]."""
    root = data_of(response)
    # At a wide span LivePokeMap sends only 100% IV Pokemon ("perfect" tier); stats may then be omitted.
    perfect_tier = root.get("span_tier") == "perfect"
    rows, seen = [], set()
    for o in root.get("pokemon") or []:
        if not isinstance(o, dict):
            continue
        ivs = (o.get("iv_attack"), o.get("iv_defense"), o.get("iv_stamina"))
        hundo = ivs == (15, 15, 15) or o.get("iv_percent") == 100
        has_stats = o.get("has_full_stats") or any(v is not None for v in ivs) or o.get("iv_percent") is not None
        if not hundo and not (perfect_tier and not has_stats):
            continue
        encounter = str(o.get("encounter_id") or o.get("encounterId") or "").strip()
        try:
            lat, lon = float(o["lat"]), float(o["lon"])
            despawn = int(o.get("despawn_ms") or 0)
        except (KeyError, TypeError, ValueError):
            continue
        if not encounter.isdigit() or encounter in seen or not (-90 <= lat <= 90 and -180 <= lon <= 180):
            continue
        if despawn and despawn < now_ms + 60_000:
            continue
        seen.add(encounter)
        level = o.get("level")
        rows.append([encounter, int(o.get("dex_id") or 0), int(o.get("form") or 0), round(lat, 6), round(lon, 6),
                     despawn, int(o.get("cp") or 0), level if isinstance(level, (int, float)) else 0])
    return rows


def write_atomic(path, payload):
    directory = os.path.dirname(path) or "."
    os.makedirs(directory, exist_ok=True)
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8") as out:
        json.dump(payload, out, separators=(",", ":"))
    os.chmod(temp, 0o644)
    os.replace(temp, path)


def read_token(path):
    try:
        with open(path, encoding="utf-8") as source:
            return source.read().strip() or None
    except OSError:
        return None


def save_token(path, token):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as out:
        out.write(token)
    os.chmod(path, 0o600)


def run_pogo(out, interval, once):
    token = os.environ.get("POGO_TOKEN", "").strip()
    if not token or any(c.isspace() for c in token):
        sys.exit("Set POGO_TOKEN to the bearer token only (without the Bearer prefix).")
    try:
        budget = int(os.environ.get("POGO_REVEALS_PER_POLL", "1"))
        lookup_results = int(os.environ.get("POGO_LOOKUP_RESULTS", "100"))
        reveal_pause = float(os.environ.get("POGO_REVEAL_PAUSE_SECONDS", "1"))
        bounds = tuple(float(v) for v in os.environ.get("POGO_BOUNDS", "-85,85,-180,180").split(","))
        if not 1 <= budget <= 100 or len(bounds) != 4 or not (-90 <= bounds[0] < bounds[1] <= 90 and -180 <= bounds[2] < bounds[3] <= 180):
            raise ValueError()
        if not 1 <= lookup_results <= 500 or not math.isfinite(reveal_pause) or not 1 <= reveal_pause <= 30:
            raise ValueError()
    except ValueError:
        sys.exit("POGO_REVEALS_PER_POLL must be 1..100; POGO_LOOKUP_RESULTS 1..500; POGO_REVEAL_PAUSE_SECONDS 1..30; POGO_BOUNDS south,north,west,east in valid ranges.")
    mode = os.environ.get("POGO_COORDINATE_MODE", "exact").strip().lower()
    if mode not in ("exact", "map"):
        sys.exit("POGO_COORDINATE_MODE must be exact or map")
    if mode == "map":
        try:
            degrees = int(os.environ.get("POGO_TILE_DEGREES", "30"))
            if degrees not in (15, 30, 45, 60):
                raise ValueError()
        except ValueError:
            sys.exit("POGO_TILE_DEGREES must be 15, 30, 45 or 60")
        collector = PogoMapCoverage(token, out, bounds, degrees,
                                    refine_dense=os.environ.get('POGO_REFINE_DENSE', '0') == '1',
                                    prioritize_active=os.environ.get('POGO_PRIORITIZE_ACTIVE', '0') == '1')
        interval = max(120, interval)
    else:
        collector = PogoLookup(token, out, budget, bounds, lookup_results, reveal_pause)
    failures = 0
    log("hundo feed starting: PoGoCoordinates %s mode, every %d s, up to %d new reveals per poll" % (mode, interval, budget if mode == "exact" else 0))
    while True:
        started = time.time()
        error = None
        try:
            rows, error = collector.poll()
            if rows or error is None:
                metadata = collector.metadata() if mode == "map" else {"coordinate_precision": "exact"}
                if mode == "map":
                    # The existing sniper treats every hundo coordinate as exact.
                    # Approximate markers belong only in a separate diagnostic list.
                    metadata['map_candidates'] = rows
                    metadata['map_coordinate_precision'] = metadata['coordinate_precision']
                    approximate = set(metadata['approximate_encounters'])
                    rows = [row for row in rows if row[0] not in approximate]
                    metadata['coordinate_precision'] = 'exact'
                write_atomic(out, {"v": 1, "fetched_ms": int(time.time() * 1000), "interval_s": interval,
                                   "source": "pogocoordinates", "bounds": bounds, "hundos": rows, **metadata})
                log("published %d hundos" % len(rows))
        except FeedError as exc:
            error = exc
        wait = interval
        if error:
            failures += 1
            wait = max(interval, error.retry_after or 300) if error.status == 429 else max(interval, min(300, interval * 2 ** min(failures, 4)))
            log("poll incomplete (%s); next try in %d s" % (error, wait))
            if error.status in (401, 403):
                log("check POGO_TOKEN and provider account access; update env and restart")
        else:
            failures = 0
        if once:
            return 1 if error else 0
        # Retry-After starts when the error is received, not when this poll began.
        time.sleep(wait if error else max(1, wait - (time.time() - started)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--once", action="store_true", help="publish one poll and exit (for validation)")
    args = parser.parse_args()
    provider = os.environ.get("HUNDO_PROVIDER", "livepokemap").strip().lower()
    if provider not in ("livepokemap", "pogocoordinates"):
        sys.exit("HUNDO_PROVIDER must be livepokemap or pogocoordinates")
    out = os.environ.get("HUNDO_OUT", "").strip()
    if not out:
        sys.exit("Set HUNDO_OUT to the published JSON file path")
    try:
        interval = max(15, int(os.environ.get("HUNDO_INTERVAL", "300" if provider == "pogocoordinates" else "30")))
    except ValueError:
        sys.exit("HUNDO_INTERVAL must be an integer number of seconds")
    if provider == "pogocoordinates":
        return run_pogo(out, interval, args.once)
    username = os.environ.get("LPM_USERNAME", "").strip()
    password = os.environ.get("LPM_PASSWORD", "")
    if not username or not password or not out:
        sys.exit("Set LPM_USERNAME, LPM_PASSWORD and HUNDO_OUT (see hundo-feed.env.example).")
    state = os.environ.get("HUNDO_STATE", "/var/lib/hundo-feed/token")
    token = read_token(state)
    failures = 0
    logged_shape = False
    tiles, polls = [WORLD], 0
    log("hundo feed starting: every %d s -> %s" % (interval, out))
    while True:
        started = time.time()
        wait = interval
        try:
            if not token:
                token = login(username, password)
                save_token(state, token)
                log("signed in to LivePokeMap")
            polls += 1
            if polls % RESET_POLLS == 0:
                tiles = [WORLD]
            try:
                responses, tiles = world(token, tiles)
            except FeedError as error:
                if error.status != 401:
                    raise
                log("session expired; signing in again")
                token = login(username, password)
                save_token(state, token)
                responses, tiles = world(token, tiles)
            now_ms = int(time.time() * 1000)
            rows, seen = [], set()
            for response in responses:
                for row in compact(response, now_ms):
                    if row[0] not in seen:  # a Pokemon on a tile edge can come back twice
                        seen.add(row[0])
                        rows.append(row)
            if not logged_shape and responses:
                root = data_of(responses[0])
                sample = (root.get("pokemon") or [None])[0]
                log("response keys %s; span_tier %s; first pokemon keys %s" % (sorted(root)[:12], root.get("span_tier"),
                    sorted(sample) if isinstance(sample, dict) else None))
                logged_shape = True
            count = len(rows)
            write_atomic(out, {"v": 1, "fetched_ms": now_ms, "interval_s": interval, "hundos": rows})
            if failures:
                log("recovered after %d failed polls" % failures)
            failures = 0
            log("published %d hundos from %d of %d map tiles" % (count, len(responses), len(tiles)))
        except FeedError as error:
            failures += 1
            if error.status == 401:
                token = None  # wrong password or revoked account: try a fresh sign-in next time
            if error.status == 429:
                wait = max(interval, error.retry_after or 60)
            else:
                wait = min(300, interval * (2 ** min(failures, 4)))  # back off on repeated failures
            log("poll failed (%s); next try in %d s" % (error, wait))
        if args.once:
            return 1 if failures else 0
        time.sleep(max(1, wait - (time.time() - started)))


if __name__ == "__main__":
    sys.exit(main())
