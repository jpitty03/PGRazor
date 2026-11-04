"""PvP keep data: a pinned PvPoke snapshot and the native table generated from it.

  py -3.12 -B tools/pvp_data.py refresh [--ref <commit>] [--source <dir>]
                                                            download PvPoke at a commit (or read a local
                                                            copy of it), rewrite the snapshot
  py -3.12 -B tools/pvp_data.py generate                    rewrite pgrazor/native/pvp_data.h from the snapshot
  py -3.12 -B tools/pvp_data.py check                       fail if pvp_data.h is stale

Only refresh touches the network. The snapshot keeps base stats, forward evolutions, the game form
name each entry should match and each species' position in PvPoke's overall Great, Ultra and Master
League rankings (normal and shadow), plus the league crests. Form matching against the game's Form enum happens on device.
"""
import argparse
import hashlib
import json
import re
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'pgrazor/native/schema/pvp'
SNAPSHOT = DATA / 'snapshot.json'
OUTPUT = ROOT / 'pgrazor/native/pvp_data.h'
ICONS = ROOT / 'pgrazor/native/pvp_icons.h'
# League crests (Great, Ultra, Master) from PokeMiners/pogo_assets, pinned in icons/provenance.json.
ICON_FILES = ('pogo_great_league.png', 'pogo_ultra_league.png', 'pogo_master_league.png')
REPO = 'pvpoke/pvpoke'
# Great, Ultra and Master League. Master's 10000 is above any CP, so every IV combination is at level 50.
LEAGUES = (1500, 2500, 10000)
# Game master CP multipliers for whole levels 1..50 (PlayerLevelSettings.CpMultiplier).
WHOLE_CPM = [
         0.0939999967813491, 0.166397869586944, 0.215732470154762, 0.255720049142837, 0.29024988412857,
         0.321087598800659, 0.349212676286697, 0.375235587358474, 0.399567276239395, 0.422500014305114,
         0.443107545375824, 0.46279838681221, 0.481684952974319, 0.499858438968658, 0.517393946647644,
         0.534354329109191, 0.550792694091796, 0.566754519939422, 0.582278907299041, 0.59740000963211,
         0.61215728521347, 0.626567125320434, 0.640652954578399, 0.654435634613037, 0.667934000492096,
         0.681164920330047, 0.694143652915954, 0.706884205341339, 0.719399094581604, 0.731700003147125,
         0.737769484519958, 0.743789434432983, 0.749761044979095, 0.75568550825119, 0.761563837528228,
         0.767397165298461, 0.77318650484085, 0.778932750225067, 0.784636974334716, 0.790300011634826,
         0.795300006866455, 0.800300002098083, 0.805299997329711, 0.81029999256134, 0.815299987792968,
         0.820299983024597, 0.825299978256225, 0.830299973487854, 0.835300028324127, 0.840300023555755]
# Half levels use the game's interpolation: sqrt((cpm(L)^2 + cpm(L+1)^2) / 2).
CPM = [m for i, cur in enumerate(WHOLE_CPM) for m in ((cur,) if i == 49 else (cur, ((cur * cur + WHOLE_CPM[i + 1] ** 2) / 2) ** 0.5))]
# PvPoke spells a few form suffixes differently from the game's Form enum.
FORM_RENAMES = [('_alolan', '_alola'), ('_paldean', '_paldea'), ('tauros_aqua', 'tauros_paldea_aqua'),
                ('tauros_blaze', 'tauros_paldea_blaze'), ('tauros_combat', 'tauros_paldea_combat'),
                ('mewtwo_armored', 'mewtwo_a'), ('zygarde', 'zygarde_fifty_percent'),
                ('zygarde_10', 'zygarde_ten_percent')]


def key(species_id):
    for old, new in FORM_RENAMES:
        if species_id == old or (old.startswith('_') and species_id.endswith(old)):
            species_id = new if not old.startswith('_') else species_id[:-len(old)] + new
            break
    return re.sub('[^a-z0-9]', '', species_id.lower())


def fetch(path, ref, source=None):
    if source:
        return (Path(source) / path).read_bytes()
    url = f'https://raw.githubusercontent.com/{REPO}/{ref}/{path}'
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read()


def resolve(ref):
    with urllib.request.urlopen(f'https://api.github.com/repos/{REPO}/commits/{ref}', timeout=60) as response:
        return json.loads(response.read())['sha']


def build(gamemaster, rankings):
    """Compact rows: [id, name, dex, atk, def, hp, base, [evolutions], [meta x3], [shadow meta x3]]."""
    entries = [e for e in gamemaster['pokemon'] if e.get('released') and not e['speciesId'].endswith('_shadow')
               and 'mega' not in e.get('tags', []) and '_mega' not in e['speciesId'] and '_primal' not in e['speciesId']]
    index = {e['speciesId']: i for i, e in enumerate(entries)}
    first = {}
    for e in entries:
        first.setdefault(e['dex'], e['speciesId'])
    positions = []
    for league in LEAGUES:
        order = {row['speciesId']: n + 1 for n, row in enumerate(rankings[league])}
        positions.append(order)
    rows = []
    for e in entries:
        sid = e['speciesId']
        stats = e['baseStats']
        evolutions = sorted({index[x] for x in e.get('family', {}).get('evolutions', []) if x in index})
        meta = [positions[i].get(sid, 0) for i in range(3)]
        shadow = [positions[i].get(sid + '_shadow', 0) for i in range(3)]
        rows.append([key(sid), e['speciesName'], e['dex'], stats['atk'], stats['def'], stats['hp'],
                     1 if first[e['dex']] == sid else 0, evolutions, meta, shadow])
    return rows


def refresh(ref, source=None):
    """With source, read a local copy of the repository at commit ref instead of downloading."""
    sha = ref if source else resolve(ref)
    gamemaster = json.loads(fetch('src/data/gamemaster.json', sha, source))
    rankings = {cp: json.loads(fetch(f'src/data/rankings/all/overall/rankings-{cp}.json', sha, source)) for cp in LEAGUES}
    license_text = fetch('LICENSE', sha, source)
    DATA.mkdir(parents=True, exist_ok=True)
    snapshot = {'source': f'https://github.com/{REPO}/tree/{sha}', 'commit': sha,
                'gamemaster_timestamp': gamemaster.get('timestamp'), 'leagues': list(LEAGUES), 'cpm': CPM,
                'species': build(gamemaster, rankings)}
    text = json.dumps(snapshot, ensure_ascii=False, separators=(',', ':')) + '\n'
    SNAPSHOT.write_text(text, encoding='utf-8', newline='\n')
    (DATA / 'LICENSE-PvPoke.txt').write_bytes(license_text)
    provenance = {'source': snapshot['source'], 'files': ['src/data/gamemaster.json'] +
                  [f'src/data/rankings/all/overall/rankings-{cp}.json' for cp in LEAGUES],
                  'snapshot_sha256': hashlib.sha256(text.encode('utf-8')).hexdigest(),
                  'species': len(snapshot['species'])}
    (DATA / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f'PvPoke {sha[:12]}: {len(snapshot["species"])} species')


def c_string(value):
    return json.dumps(value, ensure_ascii=False)


def render(snapshot):
    rows = snapshot['species']
    assert len(snapshot['cpm']) == 99 and abs(snapshot['cpm'][78] - 0.7903) < 1e-4
    assert len(rows) < 4000
    out = ['// Generated by tools/pvp_data.py from ' + snapshot['source'] + ' (MIT, see schema/pvp/LICENSE-PvPoke.txt).',
           '#pragma once', '#include <cstdint>', 'namespace pgr_pvp_data {',
           '// key: normalized game form name; meta/shadow: PvPoke overall ranking position (0 = unranked).',
           'struct Species {const char *key,*name;uint16_t dex,attack,defense,stamina,evolution,evolutions;'
           'uint16_t meta[3],shadow[3];uint8_t base;};', 'static const Species species[]={']
    edges = []
    for k, name, dex, a, d, s, base, evolutions, meta, shadow in rows:
        assert re.fullmatch('[a-z0-9]+', k) and 0 < dex < 2000 and all(0 < v < 1000 for v in (a, d, s))
        out.append('{%s,%s,%d,%d,%d,%d,%d,%d,{%d,%d,%d},{%d,%d,%d},%d},' %
                   (c_string(k), c_string(name), dex, a, d, s, len(edges), len(evolutions), *meta, *shadow, base))
        edges.extend(evolutions)
    out += ['};', 'static const uint16_t edges[]={' + (','.join(map(str, edges)) or '0') + '};',
            'static const double cpm[99]={' + ','.join(repr(v) for v in snapshot['cpm']) + '};',
            'constexpr int league_cap[3]={' + ','.join(map(str, snapshot['leagues'])) + '};',
            '}  // namespace pgr_pvp_data', '']
    return '\n'.join(out)


def render_icons():
    provenance = json.loads((DATA / 'icons/provenance.json').read_text(encoding='utf-8'))
    pins = {item['name']: item['sha256'] for item in provenance['files']}
    out = ['// Generated by tools/pvp_data.py: league crests from ' + provenance['repository'] + ' at ' +
           provenance['commit'] + ". Pokemon GO artwork, not covered by PGRazor's MIT license.",
           '#pragma once', 'namespace pgr_pvp_icons {']
    names = []
    for n, name in enumerate(ICON_FILES):
        data = (DATA / 'icons' / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != pins.get(name):
            raise SystemExit(f'{name} does not match its pinned SHA-256')
        assert data[:8] == b'\x89PNG\r\n\x1a\n'
        rows = [','.join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32)]
        out.append(f'static const unsigned char icon{n}[{len(data)}]={{' + ',\n'.join(rows) + '};')
        names.append(f'icon{n}')
    out += ['// Index 0 Great League, 1 Ultra League, 2 Master League.',
            'static const unsigned char *const png[]={' + ','.join(names) + '};',
            'static const unsigned int size[]={' + ','.join(f'sizeof({x})' for x in names) + '};',
            '}  // namespace pgr_pvp_icons', '']
    return '\n'.join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('refresh', 'generate', 'check'))
    parser.add_argument('--ref', default='master')
    parser.add_argument('--source', help='local copy of the PvPoke repository at --ref (a full commit SHA)')
    args = parser.parse_args()
    if args.command == 'refresh':
        if args.source and not re.fullmatch('[0-9a-f]{40}', args.ref):
            parser.error('--source needs --ref set to the full commit SHA of that copy')
        refresh(args.ref, args.source)
    snapshot = json.loads(SNAPSHOT.read_text(encoding='utf-8'))
    outputs = {OUTPUT: render(snapshot), ICONS: render_icons()}
    if args.command == 'check':
        for path, text in outputs.items():
            if not path.is_file() or path.read_text(encoding='utf-8') != text:
                raise SystemExit(f'{path.name} is stale; run tools/pvp_data.py generate')
        print('PvP data matches the snapshot')
        return
    for path, text in outputs.items():
        path.write_text(text, encoding='utf-8', newline='\n')
        print(f'Wrote {path.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
