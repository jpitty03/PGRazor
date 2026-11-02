"""Binary-manifest preservation, malformed-input and SDK parser tests."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pgrazor_axml', ROOT / 'axml.py')
axml = importlib.util.module_from_spec(spec)
spec.loader.exec_module(axml)


def chunk(kind, payload, header=8):
    return struct.pack('<HHI', kind, header, 8+len(payload))+payload


def node(kind, payload):
    return chunk(kind, struct.pack('<II', 1, 0xffffffff)+payload, 16)


def fixture(utf8=True, styled=False, split=False):
    strings = ['manifest', 'package', 'dev.example.test', 'application', 'b', 'split', 'config.en', 'x'*200, '\U0001f642']
    offsets = []
    content = b''
    def length(n):
        if not utf8: return struct.pack('<H', n)
        return bytes([n]) if n < 128 else bytes([0x80 | (n >> 8), n & 255])
    for value in strings:
        offsets.append(len(content))
        encoded = value.encode('utf-8' if utf8 else 'utf-16-le')
        content += length(len(value.encode('utf-16-le'))//2)
        if utf8: content += length(len(encoded))
        content += encoded+(b'\0' if utf8 else b'\0\0')
    content += b'\0'*(-len(content)%4)
    start = 28+4*(len(strings)+int(styled))
    styles = struct.pack('<IIIIII', 4, 0, 2, 0xffffffff, 0xffffffff, 0xffffffff) if styled else b''
    pool = chunk(1, struct.pack('<IIIII',len(strings),int(styled),1|(0x100 if utf8 else 0),start,start+len(content) if styled else 0)
                 +struct.pack('<'+'I'*len(offsets),*offsets)+(struct.pack('<I',0) if styled else b'')+content+styles,28)
    def attr(name, value): return struct.pack('<IIIHBBI',0xffffffff,name,value,8,0,3,value)
    attrs = attr(1,2)+(attr(5,6) if split else b'')
    manifest = node(0x102,struct.pack('<IIHHHHHH',0xffffffff,0,20,20,1+int(split),0,0,0)+attrs)
    application = node(0x102,struct.pack('<IIHHHHHH',0xffffffff,3,20,20,0,0,0,0))
    return chunk(3,pool+manifest+application+node(0x103,struct.pack('<II',0xffffffff,3))+node(0x103,struct.pack('<II',0xffffffff,0)))


def fixture_with_log_activity(exported=False, intent_filter=False):
    original=fixture()
    expanded=axml.patch_manifest(original)
    _,chunks,pool,_,resources=axml._parse(expanded)
    ns=pool.add(axml.ANDROID_NS)
    activity=pool.add('activity')
    name=pool.add(axml.LOG_ACTIVITY)
    attributes=struct.pack('<IIIHBBI',ns,pool.strings.index('name'),name,8,0,3,name)
    if exported is not None:
        attributes+=struct.pack('<IIIHBBI',ns,pool.strings.index('exported'),0xffffffff,8,0,0x12,0xffffffff if exported else 0)
    extra=node(0x102,struct.pack('<IIHHHHHH',0xffffffff,activity,20,20,1+int(exported is not None),0,0,0)+attributes)
    if intent_filter:
        tag=pool.add('intent-filter')
        extra+=node(0x102,struct.pack('<IIHHHHHH',0xffffffff,tag,20,20,0,0,0,0))
        extra+=node(0x103,struct.pack('<II',0xffffffff,tag))
    extra+=node(0x103,struct.pack('<II',0xffffffff,activity))
    _,old_chunks,_,_,_=axml._parse(original)
    original_nodes=[raw for kind,raw in old_chunks if kind>=0x100]
    resource_chunk=next(raw for kind,raw in chunks if kind==0x180)
    return chunk(3,pool.build()+resource_chunk+b''.join(original_nodes[:2])+extra+b''.join(original_nodes[2:]))


class AxmlTests(unittest.TestCase):
    def test_google_callback_opt_in_preserves_components(self):
        original = axml.patch_manifest(fixture())
        before = axml.inspect_manifest(original)
        patched = axml.add_google_browser_callback(original)
        after = axml.inspect_manifest(patched)
        self.assertEqual(after['providers'], before['providers'])
        self.assertEqual(after['activities'][:-1], before['activities'])
        self.assertEqual(after['activities'][-1]['name'], axml.GOOGLE_AUTH_ACTIVITY)
        self.assertTrue(after['activities'][-1]['exported'])
        _, chunks, pool, _, _ = axml._parse(patched)
        rows = list(axml._walk(chunks, pool))
        self.assertTrue(any(tag == 'data' and attrs.get((axml.ANDROID_NS, 'scheme')) == axml.GOOGLE_AUTH_SCHEME
                            for _, event, tag, attrs, _ in rows if event == 'start'))
        with self.assertRaisesRegex(ValueError, 'already exists'):
            axml.add_google_browser_callback(patched)

    def test_version_and_min_sdk_inspection(self):
        data=fixture()
        self.assertIsNone(axml.inspect_manifest(data)['versionCode'])
        self.assertIsNone(axml.inspect_manifest(data)['versionName'])
        self.assertIsNone(axml.inspect_manifest(data)['minSdkVersion'])
        _,chunks,pool,_,_=axml._parse(data)
        ns=pool.add(axml.ANDROID_NS)
        version=pool.add('versionCode')
        sdk=pool.add('minSdkVersion')
        uses=pool.add('uses-sdk')
        version_attr=struct.pack('<IIIHBBI',ns,version,0xffffffff,8,0,0x10,123456)
        name_value=pool.add('0.435.1')
        name_attr=struct.pack('<IIIHBBI',ns,pool.add('versionName'),name_value,8,0,3,name_value)
        root=bytearray(chunks[1][1])
        struct.pack_into('<H',root,28,3)
        root.extend(version_attr+name_attr)
        struct.pack_into('<I',root,4,len(root))
        sdk_attr=struct.pack('<IIIHBBI',ns,sdk,0xffffffff,8,0,0x10,28)
        sdk_node=node(0x102,struct.pack('<IIHHHHHH',0xffffffff,uses,20,20,1,0,0,0)+sdk_attr)
        sdk_node+=node(0x103,struct.pack('<II',0xffffffff,uses))
        rebuilt=chunk(3,pool.build()+bytes(root)+sdk_node+b''.join(raw for _,raw in chunks[2:]))
        info=axml.inspect_manifest(rebuilt)
        self.assertEqual(info['versionCode'],123456)
        self.assertEqual(info['versionName'],'0.435.1')
        self.assertEqual(info['minSdkVersion'],28)
        patched=axml.inspect_manifest(axml.patch_manifest(rebuilt))
        self.assertEqual(patched['versionCode'],123456)
        self.assertEqual(patched['versionName'],'0.435.1')
        self.assertEqual(patched['minSdkVersion'],28)

    def test_encodings_and_styles_preserved(self):
        for utf8 in (False,True):
            for styled in (False,True):
                with self.subTest(utf8=utf8,styled=styled):
                    before=fixture(utf8,styled)
                    after=axml.patch_manifest(before)
                    info=axml.inspect_manifest(after)
                    self.assertEqual(info['package'],'dev.example.test')
                    self.assertEqual(info['providers'],[{'name':axml.PROVIDER,'exported':False,'authorities':'dev.example.test.pgrazor.bootstrap'}])
                    self.assertEqual(info['activities'],[{'name':axml.LOG_ACTIVITY,'exported':False}])
                    _,old_chunks,old_pool,_,_=axml._parse(before)
                    _,new_chunks,new_pool,_,_=axml._parse(after)
                    self.assertEqual(new_pool.strings[:old_pool.count],old_pool.strings)
                    self.assertEqual(new_pool.offsets[:old_pool.count],old_pool.offsets)
                    old_bytes=old_pool.data[old_pool.start:old_pool.style_start or len(old_pool.data)]
                    self.assertEqual(new_pool.data[new_pool.start:new_pool.start+len(old_bytes)],old_bytes)
                    self.assertEqual(new_pool.flags & 1,0)
                    if styled:
                        self.assertEqual(old_pool.data[old_pool.style_start:],new_pool.data[new_pool.style_start:])
                    # Every old XML node is byte-for-byte preserved in order.
                    old_nodes=[raw for kind,raw in old_chunks if kind>=0x100]
                    new_nodes=[raw for kind,raw in new_chunks if kind>=0x100]
                    cursor=0
                    for raw in old_nodes:
                        cursor=new_nodes.index(raw,cursor)+1

    def test_repeat_patch_rejected(self):
        with self.assertRaisesRegex(ValueError,'already exists'):
            axml.patch_manifest(axml.patch_manifest(fixture()))

    def test_existing_unexported_log_activity_preserved(self):
        original=fixture_with_log_activity()
        patched=axml.patch_manifest(original)
        before=axml.inspect_manifest(original)
        after=axml.inspect_manifest(patched)
        self.assertEqual(before['activities'],after['activities'])
        self.assertEqual(len(after['activities']),1)
        self.assertEqual(len(after['providers']),1)

    def test_conflicting_log_activity_rejected(self):
        for exported,intent_filter in ((True,False),(None,False),(False,True)):
            with self.subTest(exported=exported,intent_filter=intent_filter),self.assertRaisesRegex(ValueError,'Conflicting log activity'):
                axml.patch_manifest(fixture_with_log_activity(exported,intent_filter))

    def test_split_rejected(self):
        self.assertEqual(axml.inspect_manifest(fixture(split=True))['split'],'config.en')
        with self.assertRaisesRegex(ValueError,'split'):
            axml.patch_manifest(fixture(split=True))

    def test_truncation_and_lengths_rejected(self):
        data=fixture()
        for size in (0,1,7,8,27,len(data)-1):
            with self.subTest(size=size),self.assertRaises(ValueError): axml.patch_manifest(data[:size])
        for offset,value in ((4,0),(4,len(data)+4),(12,0xfffffffc),(28,0xfffffffc)):
            bad=bytearray(data)
            struct.pack_into('<I',bad,offset,value)
            with self.subTest(offset=offset),self.assertRaises(ValueError): axml.patch_manifest(bytes(bad))

    def test_invalid_attribute_extent_rejected(self):
        data=bytearray(fixture())
        chunks=list(axml._chunks(data,8))
        element=8+len(chunks[0][1])
        struct.pack_into('<H',data,element+28,65535)
        with self.assertRaisesRegex(ValueError,'attribute'): axml.patch_manifest(bytes(data))

    def test_utf16_long_length(self):
        data=fixture(False)
        _,_,pool,_,_=axml._parse(data)
        pool.add('z'*33000)
        rebuilt=axml._Pool(pool.build())
        self.assertEqual(rebuilt.strings[-1],'z'*33000)

    def test_real_apks_and_aapt(self):
        sdk=Path(os.environ.get('ANDROID_HOME',str(Path(os.environ.get('LOCALAPPDATA',''))/'Android/Sdk')))
        aapt=sdk/'build-tools'/os.environ.get('PGRAZOR_BUILD_TOOLS','36.1.0')/'aapt.exe'
        if not aapt.is_file(): self.skipTest('Installed Android aapt unavailable')
        folder=ROOT/'build/axml-tests'
        folder.mkdir(parents=True,exist_ok=True)
        android=sdk/'platforms'/os.environ.get('PGRAZOR_ANDROID_PLATFORM','android-36.1')/'android.jar'
        if not android.is_file(): self.skipTest('Installed Android platform unavailable')
        fixture_apk=folder/'own-manifest-fixture.apk'
        subprocess.run([str(aapt),'package','-f','-M',str(ROOT/'AndroidManifest.xml'),'-I',str(android),'-F',str(fixture_apk)],check=True,capture_output=True)
        tested=0
        for apk in [fixture_apk]:
            if not apk.is_file(): continue
            with self.subTest(apk=apk.name),zipfile.ZipFile(apk) as source:
                original=source.read('AndroidManifest.xml')
                patched=axml.patch_manifest(original)
                before=axml.inspect_manifest(original)
                after=axml.inspect_manifest(patched)
                self.assertEqual(before['package'],after['package'])
                self.assertEqual(before['providers'],after['providers'][:-1])
                if any(a['name']==axml.LOG_ACTIVITY for a in before['activities']):
                    self.assertEqual(before['activities'],after['activities'])
                else:
                    self.assertEqual(before['activities'],after['activities'][:-1])
                _,_,old_pool,_,old_resources=axml._parse(original)
                _,_,new_pool,_,new_resources=axml._parse(patched)
                self.assertEqual(old_resources,new_resources[:len(old_resources)])
                self.assertEqual(old_pool.strings,new_pool.strings[:old_pool.count])
                copy=folder/(apk.stem+'-manifest-test.apk')
                # aapt xmltree only needs the manifest, not an installed APK.
                with zipfile.ZipFile(copy,'w',zipfile.ZIP_DEFLATED) as out:
                    out.writestr('AndroidManifest.xml',patched)
                result=subprocess.run([str(aapt),'dump','xmltree',str(copy),'AndroidManifest.xml'],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertIn(axml.PROVIDER,result.stdout)
                self.assertIn(axml.LOG_ACTIVITY,result.stdout)
                self.assertIn(before['package']+'.pgrazor.bootstrap',result.stdout)
                (folder/(apk.stem+'-xmltree.txt')).write_text(result.stdout,encoding='utf-8')
                tested+=1
        self.assertGreater(tested,0,'No real manifest fixture available')


if __name__=='__main__': unittest.main()
