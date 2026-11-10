"""Exercise the actual offline lookup and clock, including upstream port parity."""
import importlib.util
import json
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LocalTimeTest(unittest.TestCase):
    def test_lookup_and_clock(self):
        compiler, runtime, node = (shutil.which(name) for name in ('javac', 'java', 'node'))
        if not compiler or not runtime:
            self.skipTest('JDK unavailable')
        spec = importlib.util.spec_from_file_location('generate_timezone_lookup', ROOT.parent/'tools/generate_timezone_lookup.py')
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        self.assertEqual(generator.generate(), (ROOT/'src/dev/pgrazor/TimeZoneLookup.java').read_text(encoding='utf-8'))
        with tempfile.TemporaryDirectory(prefix='pgrazor-local-time-') as temp:
            work = Path(temp)
            sources = [ROOT/'src/dev/pgrazor'/name for name in ('TimeZoneLookup.java','LocalTimeClock.java')]
            sources.append(ROOT/'test/LocalTimeClockTest.java')
            built = subprocess.run([compiler,'-encoding','UTF-8','-d',temp,*map(str,sources)],capture_output=True,text=True,timeout=30)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            args = []
            if node:
                rng = random.Random(4352)
                points = [[rng.uniform(-90,90),rng.uniform(-180,180)] for _ in range(1500)]
                points += [[lat,lon] for lat in (-90,0,90) for lon in (-180,0,180)]
                script = 'const lookup=require(process.argv[1]);const points=JSON.parse(require("fs").readFileSync(0,"utf8"));for(const [lat,lon] of points)console.log([lat,lon,lookup(lat,lon)].join("\\t"));'
                original = subprocess.run([node,'-e',script,str(ROOT/'vendor/tz-lookup/tz.js')],input=json.dumps(points),capture_output=True,text=True,timeout=15)
                self.assertEqual(original.returncode,0,original.stderr)
                expected = work/'upstream.tsv'
                expected.write_text(original.stdout,encoding='utf-8')
                args = [str(expected)]
            tested = subprocess.run([runtime,'-cp',temp,'dev.pgrazor.LocalTimeClockTest',*args],capture_output=True,text=True,timeout=15)
            self.assertEqual(tested.returncode,0,tested.stdout+tested.stderr)
            print(tested.stdout.strip())


if __name__ == '__main__':
    unittest.main()
