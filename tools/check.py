"""Run every device-free test; naming conventions keep new suites in CI."""
from pathlib import Path
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'pgrazor'
sys.path.insert(0, str(PROJECT))
from script_bundle import bundle


def run(*args, **kwargs):
    subprocess.run([str(x) for x in args], cwd=ROOT, check=True, **kwargs)


def main():
    run(sys.executable, '-B', ROOT / 'tools/native_routes_schema.py', '--check')
    run(sys.executable, '-B', ROOT / 'tools/native_gift_schema.py', '--check')
    run(sys.executable, '-B', PROJECT / 'build.py', '--test-only')
    run(sys.executable, '-B', '-m', 'unittest', 'discover', '-s', PROJECT / 'test', '-p', 'test_*.py')
    for path in sorted((PROJECT / 'test').glob('*-test.cjs')):
        print('Checking', path.relative_to(ROOT), flush=True)
        run('node', path)
    for path in [PROJECT / 'embedded-agent.js',
                 *sorted((PROJECT / 'diagnostics').glob('*.js')),
                 *sorted((PROJECT / 'runtime').rglob('*.js'))]:
        run('node', '--check', path)
    # Parse retained research bundles; production has no JavaScript assets.
    for name in json.loads((PROJECT / 'runtime/bundles.json').read_text(encoding='utf-8')):
        run('node', '--check', input=bundle(PROJECT, name))


if __name__ == '__main__':
    main()
