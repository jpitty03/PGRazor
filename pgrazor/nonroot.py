"""Package embedded controls without replacing or installing an existing game.

--probe builds/installs only our preview to test non-root startup and hooks.
--build-probe builds a preview APK for a chosen phone ABI, without adb.
--apk INPUT builds a separate signed APK/set; it NEVER installs the game.
"""
import argparse
import contextlib
import copy
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import uuid
import zipfile

import axml
import build
import runtime_assets
import script_bundle

ROOT=Path(__file__).resolve().parent
NIANTIC_CERT_SHA256='944bda466eaf64be1c93896d5c8e9a454a07c8d8fcbc4adeeefb3545b335e979'

def require_clean_game_certificate(digests):
    if digests != [NIANTIC_CERT_SHA256]:
        raise ValueError('Game input is not signed with the verified Niantic certificate. Use a clean original APK/APKM; modified PGSharp/debug-signed builds are not a valid baseline.')

def command(*args, capture=False):
    return subprocess.run([str(a) for a in args],check=True,text=True,encoding='utf-8',errors='replace',capture_output=capture)


def cleanup_generated_builds():
    """Prune old generated APKs after a successful build, keeping pinned rollbacks."""
    try:
        retention=ROOT/'build-retention.json'
        if not retention.exists():
            config=json.loads((ROOT/'build-retention.example.json').read_text(encoding='utf-8-sig'))
            completed=sorted(p.parent for p in (ROOT/'build/nonroot').glob('*/report.json') if re.fullmatch(r'\d{8}-\d{6}-[0-9a-f]{8}',p.parent.name))
            if completed: config['pinnedRuns']=[{'run':completed[-1].name,'reason':'Initial local candidate; not device-tested. Add deployed and tested rollback pins before replacing.'}]
            retention.write_text(json.dumps(config,indent=2)+'\n',encoding='utf8')
        subprocess.run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass',
            '-File',str(ROOT/'cleanup.ps1'),'-Apply'],check=True,timeout=300)
    except (OSError,subprocess.SubprocessError) as error:
        print('WARNING: APK packaging succeeded, but automatic cleanup did not finish: '+str(error),file=sys.stderr,flush=True)

def validated_build():
    receipt=ROOT/'build/build-state.json'
    if not receipt.is_file(): raise RuntimeError('Build receipt missing. Run without --skip-build to compile the current source.')
    state=json.loads(receipt.read_text(encoding='utf-8'))
    actual=build.source_fingerprints(ROOT)
    if state.get('sources')!=actual: raise RuntimeError('Built classes are stale. Run without --skip-build.')
    if state.get('version')!=build.release_version(ROOT): raise RuntimeError('Built version is stale. Run without --skip-build.')
    dex=ROOT/'build/pgrazor.dex'
    if not dex.is_file() or runtime_assets.sha256(dex)!=state.get('dex_sha256'): raise RuntimeError('Built DEX checksum mismatch')
    data=dex.read_bytes()
    if not data.startswith(b'dex\n') or b'Ldev/pgrazor/BootstrapProvider;' not in data:
        raise RuntimeError('DEX does not contain the embedded bootstrap')
    return dex

def input_apks(source,work):
    """Extract APK sets using generated filenames, never archive paths."""
    with zipfile.ZipFile(source) as archive:
        if len(archive.namelist())!=len(set(archive.namelist())): raise ValueError('Duplicate ZIP entries in input')
        if 'AndroidManifest.xml' in archive.namelist(): return [source]
        entries=[info for info in archive.infolist() if info.filename.lower().endswith('.apk')]
        if not entries or len(entries)>100: raise ValueError('Expected an APK or a bundle with 1–100 APKs')
        if sum(info.file_size for info in entries)>2_000_000_000: raise ValueError('APK bundle exceeds 2 GB extraction limit')
        result=[]
        for i,info in enumerate(entries):
            path=work/f'input-{i}.apk'
            with archive.open(info) as reader,path.open('wb') as writer: shutil.copyfileobj(reader,writer)
            result.append(path)
        return result

def inspect_set(apks):
    rows=[]; abis=set()
    for path in apks:
        with zipfile.ZipFile(path) as archive:
            names=archive.namelist()
            if len(names)!=len(set(names)): raise ValueError('Duplicate ZIP members in '+str(path))
            info=axml.inspect_manifest(archive.read('AndroidManifest.xml'))
            abis.update(name.split('/')[1] for name in names if name.startswith('lib/') and name.endswith('.so') and len(name.split('/'))>=3)
            rows.append((path,info))
    packages={info['package'] for _,info in rows}
    bases=[path for path,info in rows if not info['split']]
    splits=[info['split'] for _,info in rows if info['split']]
    if len(packages)!=1 or len(bases)!=1 or len(splits)!=len(set(splits)): raise ValueError('Expected one base and unique splits for the same package')
    if len({info['versionCode'] for _,info in rows})!=1: raise ValueError('Split APK version codes do not match')
    base_info=next(info for path,info in rows if path==bases[0])
    try: minimum=int(base_info['minSdkVersion'])
    except (ValueError,TypeError): raise ValueError('Input must declare a numeric minimum SDK of at least 28')
    if minimum<28: raise ValueError('This module requires a base APK with minimum SDK >= 28')
    return packages.pop(),bases[0],rows,abis

def signature_entry(name):
    upper=name.upper()
    return upper=='STAMP-CERT-SHA256' or (upper.startswith('META-INF/') and (upper.endswith(('.SF','.RSA','.DSA','.EC')) or upper=='META-INF/MANIFEST.MF'))

def rewrite_apk(source,destination,*,base=False,dex=None,assets=None,preview=False,google_browser_login=False,dex_replacements=None):
    with zipfile.ZipFile(source) as original,zipfile.ZipFile(destination,'w') as output:
        names=original.namelist()
        # Bundled map-icon art may already ride along in our own preview input; any other pre-existing
        # PGRazor asset means the input was already instrumented and must be refused.
        if any(name.startswith('assets/pgrazor/') and not (preview and name.startswith(('assets/pgrazor/map-icons/','assets/pgrazor/feed-icons/pokemon/'))) for name in names): raise ValueError('Input already has PGRazor assets')
        if base:
            indices=[int(match.group(1) or '1') for name in names if (match:=re.fullmatch(r'classes(\d*)\.dex',name))]
            nextdex='classes'+str(max(indices,default=0)+1)+'.dex'
            if not indices: raise ValueError('Base APK contains no classes.dex')
            if not preview:
                for name in names:
                    if re.fullmatch(r'classes\d*\.dex',name) and b'Ldev/pgrazor/Control;' in original.read(name):
                        raise ValueError('Input already contains PGRazor classes')
        for item in original.infolist():
            if signature_entry(item.filename): continue
            if base and dex_replacements and item.filename in dex_replacements:
                output.write(dex_replacements[item.filename],item.filename,compress_type=zipfile.ZIP_DEFLATED)
                continue
            if base and assets and item.filename in assets: continue  # re-added from the current asset set below; prevents duplicate entries
            if base and item.filename=='AndroidManifest.xml':
                manifest=axml.patch_manifest(original.read(item))
                if google_browser_login: manifest=axml.add_google_browser_callback(manifest)
                # Cold-process account switches need the isolated restart helper in every base build;
                # an exactly-matching definition (our preview manifest) is accepted and left unchanged.
                manifest=axml.add_account_restart_activity(manifest)
                output.writestr(copy.copy(item),manifest); continue
            with original.open(item) as reader,output.open(copy.copy(item),'w') as writer: shutil.copyfileobj(reader,writer)
        if base:
            if not preview: output.write(dex,nextdex,compress_type=zipfile.ZIP_DEFLATED)
            for name,path_or_bytes in (assets or {}).items():
                if isinstance(path_or_bytes,bytes): output.writestr(name,path_or_bytes,compress_type=zipfile.ZIP_DEFLATED)
                else: output.write(path_or_bytes,name,compress_type=zipfile.ZIP_DEFLATED)

def validate_survey_options(enabled, panel_only, abi, flag='--il2cpp-survey'):
    if enabled and (panel_only or abi not in ('arm64-v8a', 'auto')):
        raise ValueError(flag+' requires the research runtime on the native arm64-v8a process; panel-only and non-ARM64 builds are unsupported')


@contextlib.contextmanager
def scoped_env(**values):
    """Set env vars for the duration of a build step, then restore prior values.

    Compiled production must guarantee the native pilot build (native_build.build_all is
    opt-in via PGRAZOR_NATIVE_PILOT=1) without leaving process-global PGRAZOR_* state
    behind for later callers or tests. Effective production config stays local to package().
    """
    saved={key:os.environ.get(key) for key in values}
    os.environ.update({key:str(value) for key,value in values.items()})
    try:
        yield
    finally:
        for key,previous in saved.items():
            if previous is None: os.environ.pop(key,None)
            else: os.environ[key]=previous


def assert_no_research_payload(names):
    """Package-time guard: a compiled production APK must carry no Frida or PGRazor JS.

    Scans ZIP/asset names and refuses the Frida Gadget, any PGRazor JavaScript asset,
    the Frida Java bridge and its notices. Research-only assets must never reach production.
    """
    for name in names:
        lowered=name.lower()
        if lowered.endswith('libfrida-gadget.so'):
            raise RuntimeError('Compiled production build must not ship the Frida Gadget: '+name)
        if lowered.startswith('assets/pgrazor/') and lowered.endswith(('.js','.mjs','.cjs','.js.map')):
            raise RuntimeError('Compiled production build must not ship PGRazor JavaScript: '+name)
        if lowered=='assets/pgrazor/java-bridge.js' or lowered.startswith('assets/pgrazor/bridge-notices/'):
            raise RuntimeError('Compiled production build must not ship Frida bridge assets: '+name)


def package(source, *,research_runtime=False,panel_only=False,download=False,preview=False,probe_abi=None,game_runtime_abi='auto',il2cpp_survey=False,camera_probe=False,camera_sample=False,camera_steering=False,scene_probe=False,scene_observe=False,interaction_probe=False,quick_summary=False,quick_ball=False,quick_close=False,google_browser_login=False):
    if quick_summary: raise ValueError('--quick-summary is retired; production gameplay is compiled by default')
    if research_runtime and panel_only: raise ValueError('--research-runtime cannot be combined with --panel-only')
    if preview and not (research_runtime or panel_only): raise ValueError('Preview requires --research-runtime or --panel-only')
    if research_runtime and (quick_close or quick_ball): raise ValueError('Research runtime does not support retired quick gameplay scripts')
    if panel_only and (quick_close or quick_ball): raise ValueError('Quick gameplay requires compiled production')
    compiled_gameplay = not research_runtime and not panel_only
    if compiled_gameplay:
        if il2cpp_survey or scene_probe or scene_observe or camera_sample or (camera_probe and not camera_steering):
            raise ValueError('Compiled gameplay does not support research survey, scene or passive camera diagnostics; use --research-runtime')
        quick_close = quick_ball = camera_steering = True
    quick_summary = compiled_gameplay
    compiled_location = compiled_gameplay or (research_runtime and os.environ.get('PGRAZOR_COMPILED_LOCATION') == '1')
    if compiled_location and not compiled_gameplay and (preview or os.environ.get('PGRAZOR_NATIVE_PILOT') != '1' or os.environ.get('PGRAZOR_COMPILED_JAVA_BRIDGES') != '1'):
        raise ValueError('Compiled Location requires a native pilot and compiled Java bridges')
    scene_probe=bool(scene_probe or scene_observe)
    if camera_sample and camera_steering: raise ValueError('--camera-steering and --camera-sample cannot be combined')
    camera_probe=bool(camera_probe or camera_sample or camera_steering)
    if interaction_probe and (il2cpp_survey or scene_probe or camera_sample or (camera_probe and not camera_steering)):
        raise ValueError('--interaction-probe can only run alone or with --camera-steering; other native diagnostics cannot be combined')
    if scene_probe and (il2cpp_survey or camera_probe): raise ValueError('--scene-probe cannot be combined with other native diagnostics or camera steering')
    if il2cpp_survey and camera_probe: raise ValueError('--camera-probe and --il2cpp-survey cannot be combined')
    validate_survey_options(il2cpp_survey,panel_only,probe_abi if preview else game_runtime_abi)
    validate_survey_options(camera_probe,panel_only,probe_abi if preview else game_runtime_abi,'--camera-probe')
    validate_survey_options(scene_probe,panel_only,probe_abi if preview else game_runtime_abi,'--scene-probe')
    validate_survey_options(interaction_probe,panel_only,probe_abi if preview else game_runtime_abi,'--interaction-probe')
    dex=validated_build()
    folder=ROOT/'build/nonroot'/(dt.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:8]); folder.mkdir(parents=True)
    inputs=input_apks(source,folder)
    package_name,base,rows,native_abis=inspect_set(inputs)
    if preview and package_name!='dev.pgrazor.preview': raise ValueError('Probe can only use our preview package')
    if preview:
        with zipfile.ZipFile(base) as archive:
            if hashlib.sha256(archive.read('classes.dex')).hexdigest()!=runtime_assets.sha256(dex):
                raise RuntimeError('Preview APK and module DEX do not match. Rebuild before packaging.')
    if not preview and package_name!='com.nianticlabs.pokemongo': raise ValueError('Expected Pokemon GO package; refusing a different input')
    if not preview and native_abis!={'arm64-v8a'}:
        raise ValueError('This game packager currently requires an ARM64-only APK/set. Found: '+repr(native_abis))
    if game_runtime_abi != 'auto' and game_runtime_abi not in runtime_assets.ARCHES: raise ValueError('Unsupported game runtime ABI')
    abis=[probe_abi] if preview else (sorted(runtime_assets.ARCHES) if game_runtime_abi == 'auto' else [game_runtime_abi])
    if preview and probe_abi not in runtime_assets.ARCHES: raise ValueError('Probe requires a supported 64-bit ABI')
    mode={'gadget':not panel_only,'version':runtime_assets.VERSION,'preferredAbi':'' if not preview and game_runtime_abi == 'auto' else abis[0], 'runtimeAbis':abis}
    # Compiled-only gameplay carries no Gadget runtime; bootstrap takes the compiled production path.
    if compiled_gameplay: mode['gadget']=False; mode['compiledGameplay']=True
    if research_runtime: mode['researchRuntime']=True
    native_pilot = compiled_gameplay or (research_runtime and os.environ.get('PGRAZOR_NATIVE_PILOT') == '1')
    if native_pilot: mode['nativePilot'] = True
    if compiled_gameplay or (research_runtime and os.environ.get('PGRAZOR_COMPILED_NATIVE_TRANSPORT')=='1'):
        if not native_pilot or preview: raise ValueError('Compiled native transport requires a production native pilot')
        mode['compiledNativeTransport']=True
    compiled_java_bridges = compiled_gameplay or (native_pilot and not preview and os.environ.get('PGRAZOR_COMPILED_JAVA_BRIDGES') == '1')
    compiled_catch_wait = compiled_java_bridges and quick_close
    if compiled_catch_wait: mode['compiledCatchWait']=True
    if compiled_location: mode['compiledLocationGetters']=True
    dex_replacements={}; dex_bridge_receipt=None
    if compiled_java_bridges:
        import dex_bridges
        dex_replacements,dex_bridge_receipt=dex_bridges.rewrite(base,folder/'java-bridges',download=download)
        mode['compiledJavaBridges']=True
    if native_pilot and os.environ.get('PGRAZOR_NATIVE_SELFTEST') == '1': mode['nativeSelftest'] = True
    if il2cpp_survey: mode['il2cppSurvey']=True
    if camera_probe: mode['cameraProbe']=True
    if camera_sample: mode['cameraSample']=True
    if camera_steering: mode['cameraSteering']=True
    if scene_probe: mode['sceneProbe']=True
    if scene_observe: mode['sceneObserve']=True
    if interaction_probe: mode['interactionProbe']=True
    if quick_summary: mode['quickSummary']=True
    if quick_close: mode['quickClose']=True
    if quick_ball: mode['quickBall']=True
    if google_browser_login: mode['googleBrowserLogin']=True
    assets={'assets/pgrazor/mode.json':json.dumps(mode).encode()}
    if native_pilot:
        import native_build
        with scoped_env(PGRAZOR_NATIVE_PILOT='1'):
            native_outputs = native_build.build_all(ROOT, abis, download=download)
        for abi, binary in native_outputs.items():
            assets[f'assets/pgrazor/{abi}/libpgrazor-core.so'] = binary
        assets['assets/pgrazor/native-notices/Dobby-LICENSE'] = native_build.ensure_dobby(ROOT)/'LICENSE'
    art_provenance = None
    if compiled_location:
        import art_build
        art_outputs = art_build.build_all(ROOT, abis)
        if set(art_outputs) != set(abis): raise RuntimeError('ART build ABI set mismatch')
        sessions = {Path(binary).resolve().parent.parent for binary in art_outputs.values()}
        if len(sessions) != 1: raise RuntimeError('ART outputs must share one build session')
        art_receipt_path = sessions.pop()/'receipt.json'
        art_receipt = json.loads(art_receipt_path.read_text(encoding='utf-8'))
        if art_receipt.get('success') is not True: raise RuntimeError('ART build receipt unsuccessful')
        libraries = {}
        for abi,binary in art_outputs.items():
            entry = art_receipt.get('abis',{}).get(abi,{})
            digest = runtime_assets.sha256(binary)
            if entry.get('sha256') != digest or Path(entry.get('stripped','')).resolve() != Path(binary).resolve() or entry.get('exports') != ['JNI_OnLoad']:
                raise RuntimeError('ART build output does not match verified receipt: '+abi)
            asset = f'assets/pgrazor/{abi}/libpgrazor-art.so'
            assets[asset] = binary
            libraries[abi] = {'asset':asset,'sha256':digest}
        names = ('LSPlant-LICENSE','DexBuilder-LICENSE','parallel-hashmap-LICENSE','XZ-Embedded-COPYING','Dobby-LICENSE')
        sources = art_receipt.get('license_sources',[])
        if len(sources) != len(names): raise RuntimeError('All five ART dependency licenses required')
        notices = []
        for name,source_path in zip(names,sources):
            path = Path(source_path)
            asset = 'assets/pgrazor/art-notices/'+name
            assets[asset] = path
            notices.append({'asset':asset,'source':str(path),'sha256':runtime_assets.sha256(path)})
        art_provenance = {'build_receipt':art_receipt,'build_receipt_sha256':runtime_assets.sha256(art_receipt_path),
                          'libraries':libraries,'notices':notices,
                          'relinking':'Retain the recorded local ART build session and pinned source trees for LGPL relinking; debug objects are not APK assets.',
                          'runtime_validation':'UNTESTED'}
    # POI marker art loaded by the Java map via AssetManager at pgrazor/map-icons/<name>.
    for icon_name,icon_path in build.map_icon_assets().items(): assets['assets/pgrazor/map-icons/'+icon_name]=icon_path
    for icon_name,icon_path in build.feed_icon_assets().items(): assets['assets/pgrazor/feed-icons/pokemon/'+icon_name]=icon_path
    certificates=[]
    for path,_ in rows:
        verified=command(build.TOOLS/'apksigner.bat','verify','--print-certs',path,capture=True)
        digests=re.findall(r'Signer #\d+ certificate SHA-256 digest: ([0-9a-fA-F]+)',verified.stdout)
        if not digests: raise RuntimeError('Could not verify input signing certificate: '+str(path))
        certificates.append(sorted(d.lower() for d in digests))
    if any(c!=certificates[0] for c in certificates): raise ValueError('APK set contains mismatched signing certificates')
    if not preview: require_clean_game_certificate(certificates[0])
    runtimes=[]
    if not panel_only and not compiled_gameplay:
        for abi in abis:
            binary,metadata=runtime_assets.gadget(abi,download=download)
            assets[f'assets/pgrazor/{abi}/libfrida-gadget.so']=binary; runtimes.append(metadata)
        bridge=runtime_assets.java_bridge()
        assets['assets/pgrazor/java-bridge.js']=bridge
        assets['assets/pgrazor/embedded-agent.js']=script_bundle.bundle(ROOT,'embedded-agent.js')
        if il2cpp_survey:
            assets['assets/pgrazor/il2cpp-survey.js']=ROOT/'diagnostics/il2cpp-survey.js'
        if camera_probe:
            assets['assets/pgrazor/camera-probe.js']=script_bundle.bundle(ROOT,'camera-probe.js')
        if scene_probe:
            assets['assets/pgrazor/scene-probe.js']=ROOT/'diagnostics/scene-probe.js'
        if interaction_probe:
            assets['assets/pgrazor/interaction-probe.js']=ROOT/'diagnostics/interaction-probe.js'
        for notice in ['NOTICE.txt','COPYING.frida-tools','bridge-provenance.json']:
            assets['assets/pgrazor/bridge-notices/'+notice]=ROOT/'vendor'/notice
    if compiled_gameplay: assert_no_research_payload(assets)
    report={'input':str(source.resolve()),'input_sha256':runtime_assets.sha256(source),'package':package_name,
        'mode':'compiled-gameplay' if compiled_gameplay else ('panel-only' if panel_only else 'research-runtime'),'original_abis':sorted(native_abis),'runtime_abi':game_runtime_abi if not preview else probe_abi,'runtime_abis':abis,
        'game_acceptance':'UNTESTED','runtime':runtimes,'outputs':[], 'input_certificate_sha256':certificates[0],
        'installation':'Not installed. Signed with local development key; cannot update a differently signed installation.'}
    if native_pilot:
        report['native_pilot'] = {
            'selftest_requested': mode.get('nativeSelftest', False),
            'libraries': {abi: json.loads((ROOT/'build/native/out'/abi/'receipt.json').read_text(encoding='utf-8')) for abi in abis},
            'gameplay_validation': 'UNTESTED',
        }
    if art_provenance is not None: report['compiled_location_getters'] = art_provenance
    if not panel_only and not compiled_gameplay:
        report['bridge_sha256']=runtime_assets.sha256(bridge)
        report['bridge_upstream_patch']=runtime_assets.BRIDGE_COMMIT
        report['agent_sha256']=hashlib.sha256(assets['assets/pgrazor/embedded-agent.js']).hexdigest()
        manifest=json.loads((ROOT/'runtime/bundles.json').read_text(encoding='utf-8'))
        report['script_bundles']={name:{
            'sha256':hashlib.sha256(assets['assets/pgrazor/'+name]).hexdigest(),
            'sources':[{'path':part,'sha256':runtime_assets.sha256(ROOT/part)} for part in parts]
        } for name,parts in manifest.items() if 'assets/pgrazor/'+name in assets}
    if il2cpp_survey and not compiled_gameplay:
        report['diagnostics']={'il2cpp_survey':{
            'source':str(ROOT/'diagnostics/il2cpp-survey.js'),
            'asset':'assets/pgrazor/il2cpp-survey.js',
            'sha256':runtime_assets.sha256(ROOT/'diagnostics/il2cpp-survey.js')}}
    if camera_probe and not compiled_gameplay:
        report['diagnostics']={'camera_probe':{
            'sampling':bool(camera_sample),
            'steering':bool(camera_steering),
            'source':str(ROOT/'diagnostics/camera-probe.js'),
            'asset':'assets/pgrazor/camera-probe.js',
            'sha256':hashlib.sha256(assets['assets/pgrazor/camera-probe.js']).hexdigest()}}
    if scene_probe and not compiled_gameplay:
        report['diagnostics']={'scene_probe':{
            'observe_runtime':bool(scene_observe),
            'source':str(ROOT/'diagnostics/scene-probe.js'),
            'asset':'assets/pgrazor/scene-probe.js',
            'sha256':runtime_assets.sha256(ROOT/'diagnostics/scene-probe.js')}}
    if interaction_probe and not compiled_gameplay:
        report.setdefault('diagnostics',{})['interaction_probe']={
            'startup_mode':'metadata-only',
            'source':str(ROOT/'diagnostics/interaction-probe.js'),
            'asset':'assets/pgrazor/interaction-probe.js',
            'sha256':runtime_assets.sha256(ROOT/'diagnostics/interaction-probe.js')}
    report['module_sha256']=runtime_assets.sha256(dex)
    if dex_bridge_receipt is not None: report['compiled_java_bridges']=dex_bridge_receipt
    for index,(path,info) in enumerate(rows):
        stem='base' if path==base else f'split-{index}'
        unsigned=folder/(stem+'-unsigned.apk'); aligned=folder/(stem+'-aligned.apk'); signed=folder/(stem+'.apk')
        rewrite_apk(path,unsigned,base=path==base,dex=dex,assets=assets,preview=preview,google_browser_login=google_browser_login,dex_replacements=dex_replacements)
        command(build.TOOLS/'zipalign.exe','-f','-P','16','4',unsigned,aligned)
        command(build.TOOLS/'apksigner.bat','sign',*build.signing_args(),'--out',signed,aligned)
        verified=command(build.TOOLS/'apksigner.bat','verify','--print-certs',signed,capture=True)
        (folder/(stem+'-certificate.txt')).write_text(verified.stdout,encoding='utf-8')
        command(build.TOOLS/'zipalign.exe','-c','-P','16','4',signed)
        with zipfile.ZipFile(signed) as archive:
            if compiled_gameplay: assert_no_research_payload(archive.namelist())
            parsed=axml.inspect_manifest(archive.read('AndroidManifest.xml'))
            if path==base and not any(p['name']==axml.PROVIDER and p['exported'] is False for p in parsed['providers']): raise RuntimeError('Packaged provider verification failed')
        report['outputs'].append({'file':str(signed),'sha256':runtime_assets.sha256(signed),'split':info['split']})
    (folder/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('Packaged:',folder,flush=True)
    print('No existing game installation was changed.',flush=True)
    return report,folder

def probe(report,folder,android,seconds=0):
    if report['package']!='dev.pgrazor.preview' or len(report['outputs'])!=1: raise RuntimeError('Probe installation restricted to our standalone preview')
    report['device']={'serial':android.serial}
    for key,prop in [('model','ro.product.model'),('android','ro.build.version.release'),('sdk','ro.build.version.sdk'),('abi','ro.product.cpu.abi')]:
        report['device'][key]=android.shell('getprop',prop).strip()
    (folder/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    android.call('install','-r',report['outputs'][0]['file'])
    report['installation']='Installed PGRazor Preview on '+android.serial+'; game installation unchanged.'
    (folder/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    # Fresh process is required to test the ContentProvider and native loader.
    android.shell('am','force-stop','dev.pgrazor.preview')
    log=folder/'device-test.txt'
    stream=subprocess.Popen(android.command('logcat','-v','threadtime','-T','1','PGRazor:I','AndroidRuntime:E','libc:F','DEBUG:F','crash_dump64:E','*:S'),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace')
    timeout=threading.Timer(seconds,stream.terminate) if seconds else None
    if timeout: timeout.start()
    try:
        android.shell('am','start','-n','dev.pgrazor.preview/dev.pgrazor.PreviewActivity')
        if report['mode']=='panel-only': print('Non-root panel-only probe launched. Location override should be unavailable.',flush=True)
        else: print('Non-root startup probe launched. Enable controls, move joystick, then pause. Expected synthetic consumer: PASS.',flush=True)
        print('Live log:',log,'(Ctrl+C stops desktop logging; the app stays installed.)',flush=True)
        with log.open('w',encoding='utf-8',buffering=1) as writer:
            for line in stream.stdout:
                writer.write(line); print(line,end='',flush=True)
    finally:
        if timeout: timeout.cancel()
        stream.terminate()
        try: stream.wait(timeout=5)
        except subprocess.TimeoutExpired: stream.kill(); stream.wait()

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    selection=parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--probe',action='store_true',help='Build and test embedded startup only in our preview; never installs a game')
    selection.add_argument('--build-probe',action='store_true',help='Build a standalone phone test APK without connecting to or installing on a device')
    selection.add_argument('--apk',type=Path,help='APK/APKM/APKS to package; game is never installed automatically')
    parser.add_argument('--abi',choices=sorted(runtime_assets.ARCHES),help='Required with --build-probe: architecture of the intended phone')
    parser.add_argument('--runtime-abi',choices=['auto',*sorted(runtime_assets.ARCHES)],help='With --apk, defaults to auto: bundle ARM64 and x86-64 runtimes and select the native process ABI at startup. Native gameplay hooks require ARM64.')
    parser.add_argument('--serial',help='With --probe, target this adb device serial; defaults to the only connected device')
    parser.add_argument('--research-runtime',action='store_true',help='Explicitly enable the research-only Gadget and diagnostic script runtime')
    parser.add_argument('--panel-only',action='store_true',help='Test embedded menu and logs without Gadget or location overrides')
    diagnostic=parser.add_mutually_exclusive_group()
    diagnostic.add_argument('--il2cpp-survey',action='store_true',help='Opt-in bounded IL2CPP metadata diagnostic; requires embedded Gadget on native ARM64')
    diagnostic.add_argument('--camera-probe',action='store_true',help='Opt-in bounded camera diagnostic; requires embedded Gadget on native ARM64')
    diagnostic.add_argument('--scene-probe',action='store_true',help='Opt-in scene metadata diagnostic; requires embedded Gadget on native ARM64')
    parser.add_argument('--scene-observe',action='store_true',help='Opt-in bounded passive scene observation; implies --scene-probe')
    parser.add_argument('--camera-sample',action='store_true',help='Opt-in bounded camera getter sampling on a verified thread; implies --camera-probe')
    parser.add_argument('--camera-steering',action='store_true',help='Opt-in camera steering support using the native ARM64 camera probe; implies --camera-probe')
    parser.add_argument('--interaction-probe',action='store_true',help='Opt-in interaction metadata probe with later user-requested observation; native ARM64, optionally with camera steering')
    parser.add_argument('--quick-close',action='store_true',help='Compatibility flag: compiled production includes quick close by default')
    parser.add_argument('--quick-ball',action='store_true',help='Compatibility flag: compiled production includes quick ball by default')
    parser.add_argument('--google-browser-login',action='store_true',help='Opt-in experimental Google browser sign-in')
    parser.add_argument('--download-runtime',action='store_true',help='Fetch pinned build dependencies and, for explicit research builds, the verified Gadget asset')
    parser.add_argument('--skip-build',action='store_true',help='Use compiled module only if source/checksums still match')
    parser.add_argument('--seconds',type=float,default=0,help='Stop probe desktop logging after this many seconds; 0 waits for Ctrl+C')
    args=parser.parse_args(argv)
    if args.research_runtime and args.panel_only: parser.error('--research-runtime cannot be combined with --panel-only')
    if (args.probe or args.build_probe) and not (args.research_runtime or args.panel_only): parser.error('Preview requires --research-runtime or --panel-only')
    if args.research_runtime and (args.quick_close or args.quick_ball): parser.error('Research runtime does not support retired quick gameplay scripts')
    if not (args.research_runtime or args.panel_only) and (args.il2cpp_survey or args.scene_probe or args.scene_observe or args.camera_sample or (args.camera_probe and not args.camera_steering)):
        parser.error('Research diagnostics require --research-runtime')
    if args.quick_close and args.panel_only: parser.error('--quick-close cannot be used with --panel-only')
    if args.interaction_probe and (args.il2cpp_survey or args.scene_probe or args.scene_observe or args.camera_sample or (args.camera_probe and not args.camera_steering)):
        parser.error('--interaction-probe can only run alone or with --camera-steering')
    if args.camera_sample and args.il2cpp_survey: parser.error('--camera-sample cannot be used with --il2cpp-survey')
    if args.camera_steering and args.il2cpp_survey: parser.error('--camera-steering cannot be used with --il2cpp-survey')
    if args.camera_steering and args.camera_sample: parser.error('--camera-steering cannot be used with --camera-sample')
    if args.scene_probe and (args.camera_sample or args.camera_steering): parser.error('--scene-probe cannot be combined with camera sampling or steering')
    if args.scene_observe and (args.il2cpp_survey or args.camera_probe or args.camera_sample or args.camera_steering): parser.error('--scene-observe cannot be combined with other native diagnostics or camera steering')
    if args.seconds<0 or (args.seconds and not args.probe): parser.error('--seconds must be nonnegative and requires --probe')
    if args.build_probe != bool(args.abi): parser.error('--build-probe and --abi must be used together')
    if args.runtime_abi and not args.apk: parser.error('--runtime-abi requires --apk')
    if args.serial is not None and (not args.probe or not args.serial.strip()): parser.error('--serial requires --probe and a nonempty device serial')
    if args.il2cpp_survey and args.panel_only: parser.error('--il2cpp-survey cannot be used with --panel-only')
    if (args.camera_probe or args.camera_sample or args.camera_steering) and args.panel_only: parser.error('Camera diagnostics cannot be used with --panel-only')
    if (args.scene_probe or args.scene_observe) and args.panel_only: parser.error('Scene diagnostics cannot be used with --panel-only')
    if args.interaction_probe and args.panel_only: parser.error('--interaction-probe cannot be used with --panel-only')
    if args.quick_ball and args.panel_only: parser.error('--quick-ball cannot be used with --panel-only')
    android=None; abi=args.abi
    if args.probe:
        import monitor
        serial=args.serial.strip() if args.serial else '127.0.0.1:16384'
        try: android=monitor.select_device(monitor.find_adb(),serial)
        except RuntimeError as error:
            raise RuntimeError('Cannot select adb device '+serial+'. Check adb devices and authorize USB debugging on the intended device. '+str(error)) from error
        abi=android.shell('getprop','ro.product.cpu.abi').strip()
        if abi not in runtime_assets.ARCHES: raise RuntimeError('Unsupported probe ABI: '+abi)
    validate_survey_options(args.il2cpp_survey,args.panel_only,abi if args.probe or args.build_probe else (args.runtime_abi or 'auto'))
    validate_survey_options(args.camera_probe or args.camera_sample or args.camera_steering,args.panel_only,abi if args.probe or args.build_probe else (args.runtime_abi or 'auto'),'--camera-probe')
    validate_survey_options(args.scene_probe or args.scene_observe,args.panel_only,abi if args.probe or args.build_probe else (args.runtime_abi or 'auto'),'--scene-probe')
    validate_survey_options(args.interaction_probe,args.panel_only,abi if args.probe or args.build_probe else (args.runtime_abi or 'auto'),'--interaction-probe')
    if not args.skip_build: command(sys.executable,ROOT/'build.py')
    preview=args.probe or args.build_probe
    source=ROOT/'build/pgrazor-preview.apk' if preview else args.apk.resolve()
    options={'game_runtime_abi':args.runtime_abi} if args.runtime_abi else {}
    if args.research_runtime: options['research_runtime']=True
    if args.il2cpp_survey: options['il2cpp_survey']=True
    if args.camera_probe: options['camera_probe']=True
    if args.camera_sample: options['camera_sample']=True
    if args.camera_steering: options['camera_steering']=True
    if args.scene_probe: options['scene_probe']=True
    if args.scene_observe: options['scene_observe']=True
    if args.interaction_probe: options['interaction_probe']=True
    if args.quick_close: options['quick_close']=True
    if args.quick_ball: options['quick_ball']=True
    if args.google_browser_login: options['google_browser_login']=True
    report,folder=package(source,panel_only=args.panel_only,download=args.download_runtime,preview=preview,probe_abi=abi,**options)
    cleanup_generated_builds()
    if args.probe: probe(report,folder,android,args.seconds)
    elif args.build_probe: print('Phone test APK:',report['outputs'][0]['file'],'\nInstall on a '+abi+' device and open PGRazor Preview. No device was changed.')
    else: print('Keep the current game installed. Review report.json before deciding where to test this differently signed build.')

if __name__=='__main__':
    try: main()
    except KeyboardInterrupt: print('\nDesktop log capture stopped.'); sys.exit(130)
    except Exception as error: print('ERROR:',error,file=sys.stderr); sys.exit(1)
