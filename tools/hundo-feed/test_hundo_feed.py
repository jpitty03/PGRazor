import json
import tempfile
import io
import urllib.error
import unittest
from pathlib import Path
from unittest.mock import patch

import hundo_feed as feed

NOW = 1_795_302_000_000


def spawn(encounter="-1", **changes):
    value = dict(encounter_id=encounter, pokemon_id=215, form=None, cp=1234, level=30,
                 iv_percent=100, raw_iv="15/15/15", despawn_at_ms=NOW + 600_000,
                 apx_lat=40.0, apx_long=-70.0, reveal_code="test-code")
    value.update(changes)
    return value


class FeedTests(unittest.TestCase):
    def test_unsigned_ids_and_expiry(self):
        row = feed.pogo_row(spawn(), NOW)
        self.assertEqual(row, ["18446744073709551615", 215, 0, None, None, NOW + 600_000, 1234, 30.0])
        for value in (spawn(encounter="18446744073709551616"), spawn(encounter=1.5),
                      spawn(encounter="-9223372036854775809"), spawn(encounter="0"),
                      spawn(despawn_at_ms=NOW + 59_999), spawn(level=float('nan')),
                      spawn(pokemon_id=0), spawn(iv_percent=98), spawn(raw_iv="15/15/14"),
                      spawn(iv_percent=None, raw_iv=None)):
            with self.subTest(value=value):
                self.assertIsNone(feed.pogo_row(value, NOW))

    def test_exact_coordinates_only(self):
        self.assertEqual(feed.revealed_coords({'coords': '41.1234567,-71.7654321'}), (41.123457, -71.765432))
        self.assertEqual(feed.revealed_coords({'lat': 41, 'lng': -71}), (41.0, -71.0))
        for value in ({'apx_lat': 40, 'apx_long': -70}, {'lat': float('nan'), 'lng': 0},
                      {'lat': 91, 'lng': 0}, {'coords': '0,181'}, None):
            with self.subTest(value=value), self.assertRaises(feed.FeedError):
                feed.revealed_coords(value)

    def test_world_regions_and_filters(self):
        tiles = feed.lookup_tiles(feed.WORLD)
        self.assertEqual(len(tiles), 8)
        self.assertEqual(sum((t[1]-t[0])*(t[3]-t[2]) for t in tiles), 170 * 360)
        for tile in tiles:
            body = feed.lookup_body(tile)
            self.assertEqual(body['filters']['iv_min'], 100)
            self.assertEqual(body['num_results'], 100)
            self.assertLessEqual(tile[1]-tile[0], 85)
            self.assertLessEqual(tile[3]-tile[2], 90)

    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    @patch.object(feed.time, 'sleep')
    def test_dedup_reveal_cache_restart_and_prune(self, sleep, clock):
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / 'hundos.json')
            collector = feed.PogoLookup('test-token', out, reveal_budget=1)
            lookup = {'mode': 'pokemon', 'results': [spawn(), spawn()]}
            calls = []
            def request(base, path, body=None, token=None):
                calls.append(path)
                return lookup if path == '/map/lookup' else {'lat': 41, 'lng': -71}
            with patch.object(feed, 'request_json', side_effect=request):
                rows, error = collector.poll()
                self.assertIsNone(error)
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0][3:5], [41, -71])
                self.assertEqual(calls.count('/reveal/test-code'), 1)
                collector.poll()
                self.assertEqual(calls.count('/reveal/test-code'), 1)
                feed.write_atomic(out, {'source': 'pogocoordinates', 'bounds': feed.WORLD, 'hundos': rows})
                restored = feed.PogoLookup('test-token', out, 1)
                restored.poll()
                self.assertEqual(calls.count('/reveal/test-code'), 1)
                lookup['results'] = []
                self.assertEqual(len(restored.poll()[0]), 1)
                clock.return_value = (NOW + 600_000) / 1000
                self.assertEqual(restored.poll()[0], [])
                # Credentials and reveal codes must never enter the published JSON.
                text = Path(out).read_text()
                self.assertNotIn('test-token', text)
                self.assertNotIn('test-code', text)

    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    @patch.object(feed.time, 'sleep')
    def test_rate_limit_stops_reveals_and_retains_partial(self, sleep, clock):
        with tempfile.TemporaryDirectory() as directory:
            collector = feed.PogoLookup('test-token', str(Path(directory) / 'out'), 10)
            lookup = {'mode': 'pokemon', 'results': [spawn('1'), spawn('2'), spawn('3')]}
            reveals = []
            def request(base, path, body=None, token=None):
                if path == '/map/lookup':
                    return lookup
                reveals.append(path)
                if len(reveals) == 1:
                    return {'lat': 41, 'lng': -71}
                raise feed.FeedError('rate limited', 429, 120)
            with patch.object(feed, 'request_json', side_effect=request):
                rows, error = collector.poll()
            self.assertEqual(len(reveals), 2)
            self.assertEqual(len(rows), 1)
            self.assertEqual(error.status, 429)
            self.assertEqual(error.retry_after, 120)

    def test_lookup_error_not_treated_as_empty_success(self):
        with tempfile.TemporaryDirectory() as directory:
            collector = feed.PogoLookup('test-token', str(Path(directory) / 'out'))
            with patch.object(feed, 'request_json', return_value={'detail': 'denied'}), self.assertRaises(feed.FeedError):
                collector.poll()

    @patch.dict(feed.os.environ, {'POGO_TOKEN': 'test-token', 'POGO_REVEALS_PER_POLL': '10', 'POGO_BOUNDS': '-85,85,-180,180'})
    def test_failed_poll_preserves_published_file_and_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'out.json'
            out.write_text('{"previous":true}')
            with patch.object(feed.PogoLookup, 'poll', side_effect=feed.FeedError('denied', 401)):
                self.assertEqual(feed.run_pogo(str(out), 90, once=True), 1)
            self.assertEqual(json.loads(out.read_text()), {'previous': True})

    @patch.dict(feed.os.environ, {'POGO_TOKEN': 'test-token', 'POGO_REVEALS_PER_POLL': '10', 'POGO_BOUNDS': '-85,85,-180,180'})
    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    def test_partial_publish_and_retry_after(self, clock):
        # The failed poll consumed 20 seconds; still wait the FULL Retry-After.
        clock.side_effect = [NOW / 1000, NOW / 1000 + 20, NOW / 1000 + 20]
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'out.json'
            row = ['123', 215, 0, 41, -71, NOW + 600_000, 1234, 30]
            with patch.object(feed.PogoLookup, 'poll', return_value=([row], feed.FeedError('rate limited', 429, 420))), \
                    patch.object(feed.time, 'sleep', side_effect=InterruptedError) as sleep, self.assertRaises(InterruptedError):
                feed.run_pogo(str(out), 90, once=False)
            self.assertEqual(sleep.call_args.args[0], 420)
            payload = json.loads(out.read_text())
            self.assertEqual(payload['v'], 1)
            self.assertEqual(payload['hundos'], [row])

    def test_livepokemap_compaction_unchanged(self):
        raw = {'span_tier': 'perfect', 'pokemon': [dict(encounter_id='123', dex_id=215, lat=41, lon=-71,
                                                     despawn_ms=NOW + 600_000)]}
        self.assertEqual(feed.compact(raw, NOW), [['123', 215, 0, 41, -71, NOW + 600_000, 0, 0]])

    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    @patch.object(feed.time, 'sleep')
    def test_map_coverage_dedup_forms_accuracy_and_restart(self, sleep, clock):
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / 'out')
            exact = ['1', 215, 0, 41, -71, NOW + 600_000, 1234, 30]
            feed.write_atomic(out, {'source': 'pogocoordinates', 'bounds': feed.WORLD, 'hundos': [exact]})
            collector = feed.PogoMapCoverage('test-token', out)
            self.assertEqual(len(collector.tiles), 72)
            self.assertEqual(sum((t[1]-t[0])*(t[3]-t[2]) for t in collector.tiles), 170*360)
            lookup = {'mode': 'pokemon', 'results': [spawn('1'), spawn('2', form='Alola'),
                spawn('2'), spawn('3', apx_lat=float('nan')), spawn('4', despawn_at_ms=NOW)]}
            with patch.object(feed, 'request_json', return_value=lookup) as request:
                rows, error = collector.poll()
            self.assertIsNone(error)
            self.assertEqual(len(rows), 2)
            self.assertEqual(collector.cache['1'][3:5], [41, -71])
            self.assertEqual(collector.cache['2'][3:5], [40, -70])
            self.assertEqual(collector.metadata()['approximate_encounters'], ['2'])
            self.assertEqual(collector.metadata()['form_names']['2'], 'Alola')
            self.assertTrue(all(call.args[1] == '/map/lookup' for call in request.call_args_list))
            feed.write_atomic(out, {'source': 'pogocoordinates', 'bounds': feed.WORLD,
                                   'hundos': rows, **collector.metadata()})
            restored = feed.PogoMapCoverage('test-token', out)
            self.assertEqual(restored.cursor, 18)
            self.assertEqual(restored.exact, {'1'})
            self.assertEqual(len(restored.cache), 2)
            self.assertEqual(set(feed.PogoLookup('test-token', out).cache), {'1'})
            clock.return_value = (NOW + 600_000) / 1000
            with patch.object(feed, 'request_json', return_value={'mode': 'pokemon', 'results': []}):
                self.assertEqual(restored.poll()[0], [])

    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    @patch.object(feed.time, 'sleep')
    def test_map_partial_results_stop_at_rate_limit(self, sleep, clock):
        with tempfile.TemporaryDirectory() as directory:
            collector = feed.PogoMapCoverage('test-token', str(Path(directory) / 'out'))
            with patch.object(feed, 'request_json', side_effect=[{'mode':'pokemon','results':[spawn()]},
                    feed.FeedError('limited', 429, 2214)]) as request:
                rows, error = collector.poll()
            self.assertEqual(len(rows), 1)
            self.assertEqual(request.call_count, 2)
            self.assertEqual(error.retry_after, 2214)
            self.assertEqual(collector.cursor, 1)
            self.assertEqual(collector.metadata()['next_region'], 1)

    def test_retry_after_in_error_body(self):
        error = urllib.error.HTTPError('https://example.invalid', 429, 'limited', {},
            io.BytesIO(b'{"detail":{"retry_after":2214}}'))
        with patch.object(feed.urllib.request, 'urlopen', side_effect=error), self.assertRaises(feed.FeedError) as caught:
            feed.request_json(feed.POGO_API, '/map/lookup', {})
        self.assertEqual(caught.exception.retry_after, 2214)

    def test_refined_grid_covers_world_without_overlap(self):
        with tempfile.TemporaryDirectory() as directory:
            collector = feed.PogoMapCoverage('test-token', str(Path(directory) / 'out'), refine_dense=True)
            self.assertEqual(len(collector.tiles), 114)
            self.assertEqual(sum((n-s)*(e-w) for s,n,w,e in collector.tiles), 170*360)
            self.assertIn((35, 50, -150, -135), collector.tiles)
            for i,(s,n,w,e) in enumerate(collector.tiles):
                for s2,n2,w2,e2 in collector.tiles[i+1:]:
                    self.assertFalse(max(s,s2)<min(n,n2) and max(w,w2)<min(e,e2))

    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    @patch.object(feed.time, 'sleep')
    def test_active_priority_keeps_discovery_and_resumes(self, sleep, clock):
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / 'out')
            c = feed.PogoMapCoverage('test-token', out, prioritize_active=True)
            c.cache['1'] = ['1', 215, 0, 40, -70, NOW+600_000, 100, 10]
            bodies = []
            def request(base, path, body, token):
                bodies.append(body)
                return {'mode':'pokemon','results':[]}
            with patch.object(feed, 'request_json', side_effect=request):
                rows, error = c.poll()
            self.assertIsNone(error)
            self.assertEqual(len(bodies), 18)
            regions = [(b['s_lat'],b['n_lat'],b['w_lng'],b['e_lng']) for b in bodies]
            self.assertEqual(regions[0], (35,65,-90,-60))
            self.assertIn(c.tiles[0], regions)
            self.assertEqual(len(set(regions)),18)
            feed.write_atomic(out,{'source':'pogocoordinates','bounds':feed.WORLD,'hundos':rows,**c.metadata()})
            restored=feed.PogoMapCoverage('test-token',out,prioritize_active=True)
            self.assertEqual(restored.cursor,c.cursor)
            self.assertEqual(restored.priority_cursor,c.priority_cursor)
            self.assertEqual(restored.selection_step,c.selection_step)

    @patch.dict(feed.os.environ, {'POGO_TOKEN':'test-token','POGO_COORDINATE_MODE':'map',
                                'POGO_BOUNDS':'-85,85,-180,180'})
    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    def test_map_markers_never_become_sniper_targets(self, clock):
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / 'out')
            exact = ['1',215,0,40,-70,NOW+600_000,100,10]
            approximate = ['2',215,0,40.1,-70.1,NOW+600_000,100,10]
            collector = feed.PogoMapCoverage('test-token',out)
            collector.cache = {'1':exact,'2':approximate}
            collector.exact = {'1'}
            with patch.object(feed,'PogoMapCoverage',return_value=collector), \
                    patch.object(collector,'poll',return_value=([exact,approximate],None)):
                self.assertEqual(feed.run_pogo(out,120,True),0)
            payload=json.loads(Path(out).read_text())
            self.assertEqual(payload['hundos'],[exact])
            self.assertEqual(payload['map_candidates'],[exact,approximate])
            self.assertEqual(payload['coordinate_precision'],'exact')
            self.assertEqual(set(feed.PogoMapCoverage('test-token',out).cache),{'1','2'})
            self.assertEqual(set(feed.PogoLookup('test-token',out).cache),{'1'})

    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    @patch.object(feed.time, 'sleep')
    def test_vip_results_spacing_and_short_lived_targets(self, sleep, clock):
        with tempfile.TemporaryDirectory() as directory:
            c=feed.PogoLookup('test-token', str(Path(directory)/'out'), reveal_budget=10,
                              lookup_results=500, reveal_pause=2)
            bodies=[]
            reveals=[]
            def request(base,path,body=None,token=None):
                if path=='/map/lookup':
                    bodies.append(body)
                    return {'mode':'pokemon','results':[spawn('1',reveal_code='a'),spawn('2',reveal_code='b'),
                            spawn('3',reveal_code='soon',despawn_at_ms=NOW+100_000)]}
                reveals.append(path)
                return {'lat':41,'lng':-71}
            with patch.object(feed,'request_json',side_effect=request):
                rows,error=c.poll()
            self.assertIsNone(error)
            self.assertTrue(all(b['num_results']==500 for b in bodies))
            self.assertEqual(len(rows),2)
            self.assertEqual(reveals,['/reveal/a','/reveal/b'])
            self.assertIn(unittest.mock.call(2),sleep.call_args_list)

    def test_unsigned_ids(self):
        self.assertEqual(feed.unsigned_id('-1'), '18446744073709551615')
        self.assertEqual(feed.unsigned_id(12), '12')
        self.assertEqual(feed.unsigned_id('0x10'), '16')
        for value in ('0', '', 'abc', True, 1.5, None, '18446744073709551616', '-9223372036854775809'):
            with self.subTest(value=value):
                self.assertIsNone(feed.unsigned_id(value))

    def test_merge_dedups_by_priority_and_drops_unusable_rows(self):
        first = feed.Scheduled('a', None, 60)
        second = feed.Scheduled('b', None, 60)
        first.rows = [['1', 215, 0, 41, -71, NOW + 600_000, 1, 1], ['2', 215, 0, None, None, NOW + 600_000, 1, 1]]
        second.rows = [['1', 215, 0, 42, -72, NOW + 600_000, 1, 1], ['3', 1, 0, 10, 10, NOW + 59_000, 1, 1],
                       ['4', 1, 0, 10, 10, NOW + 600_000, 1, 1], ['5', 1, 0, 91, 10, NOW + 600_000, 1, 1]]
        rows, spots, counts = feed.merge([first, second], NOW)
        self.assertEqual(spots, [])
        self.assertEqual([r[0] for r in rows], ['1', '4'])
        self.assertEqual(rows[0][3:5], [41, -71])
        self.assertEqual(counts['a'], {'active': 1, 'unique': 1, 'status': 'starting'})
        self.assertEqual(counts['b'], {'active': 2, 'unique': 1, 'status': 'starting'})

    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    @patch.object(feed.time, 'sleep')
    def test_known_rows_skip_reveals(self, sleep, clock):
        with tempfile.TemporaryDirectory() as directory:
            collector = feed.PogoLookup('test-token', str(Path(directory) / 'out'), 10)
            reveals = []
            def request(base, path, body=None, token=None):
                if path == '/map/lookup':
                    return {'mode': 'pokemon', 'results': [spawn('1', reveal_code='a'), spawn('2', reveal_code='b')]}
                reveals.append(path)
                return {'lat': 41, 'lng': -71}
            with patch.object(feed, 'request_json', side_effect=request):
                rows, error = collector.poll({'1': ['1', 215, 0, 40, -70, NOW + 600_000, 0, 0]})
            self.assertEqual(reveals, ['/reveal/b'])
            self.assertEqual([r[0] for r in rows], ['2'])

    def test_one_source_cooldown_does_not_stall_others(self):
        class Limited:
            def poll(self, known):
                return [['1', 215, 0, 41, -71, NOW + 3_600_000, 0, 0]], feed.FeedError('limited', 429, 45_360)
        class Healthy:
            polls = 0
            def poll(self, known):
                self.polls += 1
                return [['1', 215, 0, 41, -71, NOW + 3_600_000, 0, 0], ['2', 1, 0, 1, 1, NOW + 3_600_000, 0, 0]], None
        clock = [NOW / 1000]
        healthy = Healthy()
        sources = [feed.Scheduled('limited', Limited(), 150), feed.Scheduled('free', healthy, 60)]
        def sleep(seconds):
            if clock[0] - NOW / 1000 > 600:
                raise InterruptedError
            clock[0] += seconds
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'out.json'
            with patch.object(feed.time, 'time', side_effect=lambda: clock[0]), \
                    patch.object(feed.time, 'sleep', side_effect=sleep), self.assertRaises(InterruptedError):
                feed.run_sources(str(out), sources, once=False)
            payload = json.loads(out.read_text())
        self.assertEqual(sources[0].next_at, NOW / 1000 + 45_360)
        self.assertGreaterEqual(healthy.polls, 10)
        self.assertEqual(payload['source'], 'merged')
        self.assertEqual([r[0] for r in payload['hundos']], ['1', '2'])
        self.assertEqual(payload['sources']['limited']['status'], 'HTTP 429')
        self.assertEqual(payload['sources']['free']['unique'], 1)

    def test_city_map_rows(self):
        raw = {'pokemons': [
            dict(pokemon_id=517, lat=40.62183846, lng=-74.11284705, despawn=(NOW + 900_000) // 1000, attack=15,
                 defence=15, stamina=15, cp=500, level=20, form=0),
            dict(pokemon_id=517, lat=40.6, lng=-74.1, despawn=(NOW + 900_000) // 1000, attack=15, defence=14,
                 stamina=15, cp=500, level=20, form=0),
            dict(pokemon_id=1, lat=40.6, lng=-74.1, despawn=(NOW + 30_000) // 1000, attack=15, defence=15, stamina=15),
            dict(pokemon_id=1, lat='x', lng=-74.1, despawn=(NOW + 900_000) // 1000, attack=15, defence=15, stamina=15)]}
        rows = feed.city_rows(raw, NOW)
        self.assertEqual(len(rows), 1)
        key, dex, form, lat, lon, despawn, cp, level = rows[0]
        self.assertRegex(key, r'^s[0-9a-f]{16}$')
        self.assertEqual((dex, form, lat, lon, despawn, cp, level), (517, 0, 40.621838, -74.112847, NOW + 900_000, 500, 20.0))
        self.assertEqual(key, feed.city_rows(raw, NOW)[0][0])  # stable across polls
        with self.assertRaises(feed.FeedError):
            feed.city_rows({'detail': 'nope'}, NOW)

    def test_spots_dedup_against_hundos_and_each_other(self):
        class Spots:
            spots = True
        hundos, city_a, city_b = feed.Scheduled('pogo', None, 60), feed.Scheduled('a', Spots(), 60), feed.Scheduled('b', Spots(), 60)
        hundos.rows = [['9', 517, 0, 40.0, -74.0, NOW + 900_000, 0, 0]]
        near = feed.spot_key(517, 40.0002, -74.0, NOW + 900_000)       # ~22 m from the hundo
        other_species = feed.spot_key(1, 40.0002, -74.0, NOW + 900_000)
        far = feed.spot_key(517, 40.01, -74.0, NOW + 900_000)          # ~1.1 km away
        city_a.rows = [[near, 517, 0, 40.0002, -74.0, NOW + 900_000, 0, 0],
                       [other_species, 1, 0, 40.0002, -74.0, NOW + 900_000, 0, 0],
                       [far, 517, 0, 40.01, -74.0, NOW + 900_000, 0, 0],
                       ['bogus', 1, 0, 40, -74, NOW + 900_000, 0, 0]]
        city_b.rows = [[feed.spot_key(517, 40.0101, -74.0, NOW + 900_000), 517, 0, 40.0101, -74.0, NOW + 900_000, 0, 0]]
        rows, spots, counts = feed.merge([hundos, city_a, city_b], NOW)
        self.assertEqual([r[0] for r in rows], ['9'])
        self.assertEqual([s[0] for s in spots], [other_species, far])
        self.assertEqual(counts['a'], {'active': 3, 'unique': 2, 'status': 'starting'})
        self.assertEqual(counts['b'], {'active': 1, 'unique': 0, 'status': 'starting'})
        self.assertFalse(feed.valid_row(spots[0], NOW))  # a spot never passes as an encounter id

    @patch.object(feed.time, 'time', return_value=NOW / 1000)
    def test_merged_file_restores_exact_cache(self, clock):
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / 'out')
            feed.write_atomic(out, {'source': 'merged', 'bounds': list(feed.WORLD),
                                   'hundos': [['7', 215, 0, 41, -71, NOW + 600_000, 0, 0]]})
            self.assertEqual(set(feed.PogoLookup('test-token', out).cache), {'7'})


if __name__ == '__main__':
    unittest.main()
