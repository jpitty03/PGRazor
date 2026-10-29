"""Archive Git-visible source; optionally include reviewed working-tree source."""
import argparse
from pathlib import Path
import subprocess
import zipfile
ROOT=Path(__file__).resolve().parents[1]
BLOCKED={'.apk','.apkm','.apks','.aab','.jks','.keystore','.p12','.key','.pem','.log','.pyc','.so','.a','.o','.dex'}
DENIED={'build','input','.artifacts','.local','hermes-swarm','frida-monitor','__pycache__','node_modules','.git'}
NEW_MODULES={'pgrazor/art_build.py','pgrazor/dex_bridges.py','pgrazor/native_build.py',
 'pgrazor/native_harness.py','pgrazor/runtime_audit.py','tools/native_routes_schema.py','tools/native_gift_schema.py','tools/prepare_native.py',
 'docs/NATIVE_MIGRATION.md','pgrazor/research/README.txt',
 'pgrazor/runtime/compiled-google-signin.js','pgrazor/runtime/compiled-location.js',
 'pgrazor/runtime/native-core.js','pgrazor/runtime/native-transport.js',
 'pgrazor/runtime/native-camera-metadata.js','pgrazor/runtime/native-camera-sampler.js'}

def allowed_addition(name):
    path=Path(name)
    return name in NEW_MODULES or (name.startswith(('pgrazor/native/','pgrazor/art/',
      'pgrazor/src/','pgrazor/test/','tools/dex_bridge/')) and
      (path.suffix.lower() in {'.cpp','.c','.h','.hpp','.inc','.java','.py','.json','.map','.md','.cjs'} or path.name=='CMakeLists.txt'))

def validate(root,name):
    path=Path(name)
    parts={part.lower() for part in path.parts}
    if path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
        raise ValueError('Unsafe archive path: '+name)
    if (path.suffix.lower() in BLOCKED or parts & DENIED or name=='pgrazor/build-retention.json' or
        any(part.startswith(('.env','login-info-')) or any(word in part for word in
          ('handoff','credential','secret','private-key')) for part in parts)):
        raise ValueError('Private/generated source refused: '+name)
    current=root
    for part in path.parts:
        current=current/part
        if current.is_symlink() or (hasattr(current,'is_junction') and current.is_junction()):
            raise ValueError('Symlink/junction source refused: '+name)
    full=root/path
    if not full.resolve().is_relative_to(root.resolve()) or not full.is_file():
        raise ValueError('Source missing or outside checkout: '+name)
    return full

def select_files(root,include_working_tree=False):
    def names(*args):
        return set(filter(None,subprocess.check_output(['git','ls-files','-z',*args],cwd=root).decode('utf-8').split('\0')))
    tracked=names('--cached')-names('--deleted')
    additions={name for name in names('--others','--exclude-standard') if allowed_addition(name)}
    if additions and not include_working_tree:
        raise ValueError('Untracked project source would be omitted; review/stage it or use --include-working-tree: '+', '.join(sorted(additions)))
    selected=tracked | (additions if include_working_tree else set())
    return [(validate(root,name),name) for name in sorted(selected)]

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--include-working-tree',action='store_true',help='Add nonignored source from native/Java/test and named build-tool allowlists')
    args=parser.parse_args(argv)
    try: files=select_files(ROOT,args.include_working_tree)
    except ValueError as error: parser.exit(1,str(error)+'\n')
    output=ROOT/'dist/PGRazor-source-tools.zip';output.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED) as archive:
        for full,name in files: archive.write(full,name)
    print('Source/tools archive:',output)

if __name__=='__main__': main()
