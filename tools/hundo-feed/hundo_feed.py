#!/usr/bin/env python3
"""PGRazor hundo feed: polls LivePokeMap's world-wide 100% IV list with one account and publishes it as a
static JSON file, so the app can read it without each user signing in.

Runs on a server (systemd: hundo-feed.service). Standard library only. Configuration comes from the
environment (see hundo-feed.env.example):

  LPM_USERNAME, LPM_PASSWORD   the LivePokeMap account
  HUNDO_OUT                    where to write the file, e.g. /var/www/example.com/pgrazor/hundos.json
  HUNDO_INTERVAL               seconds between polls (default 30, at least 15)
  HUNDO_STATE                  where the session token is kept (default /var/lib/hundo-feed/token)

The file is a compact list, written atomically (temp file + rename):

  {"v": 1, "fetched_ms": <ms>, "interval_s": 30,
   "hundos": [[encounter_id, dex, form, lat, lon, despawn_ms, cp, level], ...]}

encounter_id is a decimal string (64-bit). Only 100% IV Pokemon (15/15/15) with at least a minute left
are kept; everything else in LivePokeMap's response (forts, stations, other fields) is dropped.
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request

BASE = "https://play.livepokemap.com"
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


def post(path, body, token=None):
    request = urllib.request.Request(BASE + path, data=json.dumps(body).encode("utf-8"), method="POST")
    request.add_header("Content-Type", "application/json; charset=utf-8")
    request.add_header("Accept", "application/json")
    request.add_header("User-Agent", USER_AGENT)
    if token:
        request.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_S) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        retry = error.headers.get("Retry-After") if error.headers else None
        try:
            retry_s = max(0, int(retry)) if retry else 0
        except ValueError:
            retry_s = 0
        raise FeedError("HTTP %d from %s" % (error.code, path), error.code, retry_s)
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise FeedError("network error on %s: %s" % (path, error))
    except json.JSONDecodeError:
        raise FeedError("non-JSON reply from %s" % path)


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


def main():
    username = os.environ.get("LPM_USERNAME", "").strip()
    password = os.environ.get("LPM_PASSWORD", "")
    out = os.environ.get("HUNDO_OUT", "").strip()
    if not username or not password or not out:
        sys.exit("Set LPM_USERNAME, LPM_PASSWORD and HUNDO_OUT (see hundo-feed.env.example).")
    interval = max(15, int(os.environ.get("HUNDO_INTERVAL", "30")))
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
        time.sleep(max(1, wait - (time.time() - started)))


if __name__ == "__main__":
    main()
