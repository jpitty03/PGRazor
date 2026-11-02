"""Small, append-only binary Android manifest editor (no external packages).

Only adds an unexported bootstrap provider and log activity. Existing XML chunks, string indices,
string bytes, resource IDs and style data remain unchanged.
"""
import struct

NO_INDEX = 0xffffffff
ANDROID_NS = 'http://schemas.android.com/apk/res/android'
PROVIDER = 'dev.pgrazor.BootstrapProvider'
LOG_ACTIVITY = 'dev.pgrazor.LogActivity'
GOOGLE_AUTH_ACTIVITY = 'dev.pgrazor.GoogleAuthActivity'
GOOGLE_AUTH_SCHEME = 'com.googleusercontent.apps.848232511240-dmrj3gba506c9svge2p9gq35p1fg654p'
RESTART_ACTIVITY = 'dev.pgrazor.AccountRestartActivity'
RESTART_PROCESS = ':pgrazor_restart'
ATTR_IDS = {'name': 0x01010003, 'exported': 0x01010010, 'authorities': 0x01010018}
RESTART_ATTR_IDS = {'name': 0x01010003, 'exported': 0x01010010, 'process': 0x01010011}


def add_google_browser_callback(data):
    """Opt-in prototype callback; preserve all existing components and filters."""
    header, chunks, pool, maps, resources = _parse(data)
    if any(a['name'] == GOOGLE_AUTH_ACTIVITY for a in inspect_manifest(data)['activities']):
        raise ValueError('Google callback activity already exists')
    ns = pool.add(ANDROID_NS)
    prefix = pool.add('android')
    attr_names = {}
    for key, resource_id in {'name': 0x01010003, 'exported': 0x01010010, 'scheme': 0x01010027}.items():
        index = next((i for i, value in enumerate(pool.strings) if value == key and i < len(resources) and resources[i] == resource_id), None)
        if index is None:
            index = len(pool.strings)
            pool.strings.append(key)
            resources.extend([0] * (index + 1 - len(resources)))
            resources[index] = resource_id
        attr_names[key] = index
    def node(kind, payload):
        return struct.pack('<HHIII', kind, 16, 16 + len(payload), 0, NO_INDEX) + payload
    def start(tag, attrs=()):
        encoded = b''
        for key, value in attrs:
            boolean = isinstance(value, bool)
            index = NO_INDEX if boolean else pool.add(value)
            encoded += struct.pack('<IIIHBBI', ns, attr_names[key], index, 8, 0, 0x12 if boolean else 3, int(value) if boolean else index)
        return node(0x102, struct.pack('<IIHHHHHH', NO_INDEX, pool.add(tag), 20, 20, len(attrs), 0, 0, 0) + encoded)
    def end(tag):
        return node(0x103, struct.pack('<II', NO_INDEX, pool.add(tag)))
    extra = [node(0x100, struct.pack('<II', prefix, ns)),
             start('activity', [('name', GOOGLE_AUTH_ACTIVITY), ('exported', True)]), start('intent-filter')]
    for tag, key, value in [('action', 'name', 'android.intent.action.VIEW'),
                             ('category', 'name', 'android.intent.category.DEFAULT'),
                             ('category', 'name', 'android.intent.category.BROWSABLE'),
                             ('data', 'scheme', GOOGLE_AUTH_SCHEME)]:
        extra.extend([start(tag, [(key, value)]), end(tag)])
    extra.extend([end('intent-filter'), end('activity'), node(0x101, struct.pack('<II', prefix, ns))])
    ends = [i for i, event, tag, _, depth in _walk(chunks, pool) if event == 'end' and tag == 'application' and depth == 1]
    if len(ends) != 1:
        raise ValueError('Application end not found')
    resource_chunk = struct.pack('<HHI', 0x180, 8, 8 + 4 * len(resources)) + struct.pack('<' + 'I' * len(resources), *resources)
    output = []
    for i, (kind, raw) in enumerate(chunks):
        if i == ends[0]: output.extend(extra)
        if i == 0:
            output.append(pool.build())
            if not maps: output.append(resource_chunk)
        elif kind == 0x180: output.append(resource_chunk)
        else: output.append(raw)
    body = b''.join(output)
    outer = bytearray(data[:header])
    struct.pack_into('<I', outer, 4, len(outer) + len(body))
    result = bytes(outer) + body
    inspect_manifest(result)
    return result


def _u16(data, offset): return struct.unpack_from('<H', data, offset)[0]
def _u32(data, offset): return struct.unpack_from('<I', data, offset)[0]


def _chunks(data, start=0, end=None):
    end = len(data) if end is None else end
    while start < end:
        if end - start < 8: raise ValueError('Truncated chunk header')
        kind, header, size = struct.unpack_from('<HHI', data, start)
        if header < 8 or size < header or size % 4 or start + size > end:
            raise ValueError('Malformed chunk length')
        yield kind, bytes(data[start:start+size])
        start += size


def _length(data, offset, utf8):
    width = 1 if utf8 else 2
    if offset + width > len(data): raise ValueError('Truncated string length')
    first = data[offset] if utf8 else _u16(data, offset)
    flag = 0x80 if utf8 else 0x8000
    offset += width
    if first & flag:
        if offset + width > len(data): raise ValueError('Truncated string length')
        second = data[offset] if utf8 else _u16(data, offset)
        first = ((first & (flag-1)) << (width*8)) | second
        offset += width
    return first, offset


def _encode_length(length, utf8):
    if utf8:
        if length > 0x7fff: raise ValueError('UTF-8 string exceeds format limit')
        return bytes([length]) if length < 0x80 else bytes([(length >> 8) | 0x80, length & 255])
    if length > 0x7fffffff: raise ValueError('UTF-16 string exceeds format limit')
    return struct.pack('<H', length) if length < 0x8000 else struct.pack('<HH', (length >> 16) | 0x8000, length & 65535)


class _Pool:
    def __init__(self, data):
        self.data = data
        self.header = _u16(data, 2)
        if self.header < 28: raise ValueError('Short string pool header')
        self.count, self.styles, self.flags, self.start, self.style_start = struct.unpack_from('<IIIII', data, 8)
        table_end = self.header + 4*(self.count+self.styles)
        limit = self.style_start or len(data)
        if not (table_end <= self.start <= limit <= len(data)):
            raise ValueError('Invalid string pool offsets')
        if self.styles and not self.style_start: raise ValueError('Missing style data')
        if self.styles > self.count: raise ValueError('Style count exceeds string count')
        for i in range(self.styles):
            offset = _u32(data, self.header+4*(self.count+i))
            if offset == NO_INDEX: continue
            pos = self.style_start+offset
            if offset % 4 or pos+4 > len(data): raise ValueError('Invalid style offset')
            while True:
                if pos+4 > len(data): raise ValueError('Unterminated style spans')
                name = _u32(data, pos)
                if name == NO_INDEX: break
                if name >= self.count or pos+12 > len(data): raise ValueError('Invalid style span')
                pos += 12
        self.utf8 = bool(self.flags & 0x100)
        self.offsets = [_u32(data, self.header+4*i) for i in range(self.count)]
        self.strings = []
        string_data = data[self.start:limit]
        for offset in self.offsets:
            units, pos = _length(string_data, offset, self.utf8)
            length = units
            if self.utf8: length, pos = _length(string_data, pos, True)
            size = length if self.utf8 else length*2
            terminator = 1 if self.utf8 else 2
            if pos + size + terminator > len(string_data) or any(string_data[pos+size:pos+size+terminator]):
                raise ValueError('Invalid string terminator or length')
            try:
                value = string_data[pos:pos+size].decode('utf-8' if self.utf8 else 'utf-16-le')
            except UnicodeError as exc: raise ValueError('Invalid string encoding') from exc
            if len(value.encode('utf-16-le'))//2 != units: raise ValueError('Invalid UTF-16 length')
            self.strings.append(value)

    def string(self, index):
        if index == NO_INDEX: return None
        if index >= len(self.strings): raise ValueError('Invalid string index')
        return self.strings[index]

    def add(self, value):
        if value in self.strings: return self.strings.index(value)
        self.strings.append(value)
        return len(self.strings)-1

    def build(self):
        if len(self.strings) == self.count: return self.data
        table_end = self.header+4*(self.count+self.styles)
        gap = self.data[table_end:self.start]
        content = bytearray(self.data[self.start:self.style_start or len(self.data)])
        offsets = list(self.offsets)
        for value in self.strings[self.count:]:
            offsets.append(len(content))
            units = len(value.encode('utf-16-le'))//2
            content.extend(_encode_length(units, self.utf8))
            encoded = value.encode('utf-8' if self.utf8 else 'utf-16-le')
            if self.utf8: content.extend(_encode_length(len(encoded), True))
            content.extend(encoded)
            content.extend(b'\0' if self.utf8 else b'\0\0')
        content.extend(b'\0' * (-len(content) % 4))
        style_offsets = self.data[self.header+4*self.count:table_end]
        styles = self.data[self.style_start:] if self.style_start else b''
        new_start = self.header+4*(len(offsets)+self.styles)+len(gap)
        header = bytearray(self.data[:self.header])
        total = new_start+len(content)+len(styles)
        struct.pack_into('<I', header, 4, total)
        struct.pack_into('<IIIII', header, 8, len(offsets), self.styles,
                         self.flags & ~1, new_start, new_start+len(content) if self.style_start else 0)
        return bytes(header)+struct.pack('<'+'I'*len(offsets), *offsets)+style_offsets+gap+content+styles


def _parse(data):
    roots = list(_chunks(data))
    if len(roots) != 1 or roots[0][0] != 3: raise ValueError('Expected one binary XML document')
    header = _u16(data, 2)
    chunks = list(_chunks(data, header))
    pools = [i for i, (kind, _) in enumerate(chunks) if kind == 1]
    if len(pools) != 1 or pools[0] != 0: raise ValueError('Expected leading string pool')
    pool = _Pool(chunks[0][1])
    maps = [i for i, (kind, _) in enumerate(chunks) if kind == 0x180]
    if len(maps) > 1: raise ValueError('Duplicate resource map')
    resources = []
    if maps:
        raw = chunks[maps[0]][1]
        if _u16(raw, 2) != 8: raise ValueError('Invalid resource map header')
        resources = [_u32(raw, p) for p in range(8, len(raw), 4)]
        if len(resources) > pool.count: raise ValueError('Resource map exceeds string count')
    return header, chunks, pool, maps, resources


def _walk(chunks, pool):
    stack = []
    for i, (kind, raw) in enumerate(chunks):
        if kind not in (0x100, 0x101, 0x102, 0x103, 0x104): continue
        header = _u16(raw, 2)
        if header < 16: raise ValueError('Invalid XML node header')
        pool.string(_u32(raw, 12))
        if kind in (0x100, 0x101, 0x103):
            if len(raw) < header+8: raise ValueError('Truncated XML node')
            pool.string(_u32(raw, header))
            name = pool.string(_u32(raw, header+4))
            if kind == 0x103:
                if not stack or stack[-1] != (pool.string(_u32(raw, header)), name):
                    raise ValueError('Unbalanced XML elements')
                yield i, 'end', name, {}, len(stack)-1
                stack.pop()
        elif kind == 0x102:
            if len(raw) < header+20: raise ValueError('Truncated element header')
            namespace = pool.string(_u32(raw, header))
            name = pool.string(_u32(raw, header+4))
            start, size, count = struct.unpack_from('<HHH', raw, header+8)
            if start < 20 or size < 20 or header+start+size*count > len(raw):
                raise ValueError('Invalid attribute bounds')
            attrs = {}
            for p in range(header+start, header+start+size*count, size):
                ns, key, value = struct.unpack_from('<III', raw, p)
                ns, key = pool.string(ns), pool.string(key)
                raw_value = pool.string(value)
                value_size, reserved, typ, value = struct.unpack_from('<HBBI', raw, p+12)
                if value_size != 8 or reserved: raise ValueError('Invalid typed value')
                if typ == 3: value = pool.string(value)
                elif typ == 0x12: value = bool(value)
                elif raw_value is not None: value = raw_value
                if (ns, key) in attrs: raise ValueError('Duplicate XML attribute')
                attrs[(ns, key)] = value
            yield i, 'start', name, attrs, len(stack)
            stack.append((namespace, name))
    if stack: raise ValueError('Unclosed XML elements')


def inspect_manifest(data):
    """Return identity, SDK requirements, and components from binary XML bytes.

    versionName/versionCode/minSdkVersion are None when the corresponding
    attribute is absent. versionCode/minSdkVersion retain a string value for
    preview/codename declarations; versionName is the declared string.
    """
    _, chunks, pool, _, _ = _parse(data)
    result = {'package': None, 'split': None, 'versionCode': None, 'versionName': None,
              'minSdkVersion': None, 'providers': [], 'activities': []}
    manifests = 0
    applications = 0
    for _, event, name, attrs, depth in _walk(chunks, pool):
        if event != 'start': continue
        if name == 'manifest' and depth == 0:
            manifests += 1
            result['package'] = attrs.get((None, 'package'))
            result['split'] = attrs.get((None, 'split'))
            result['versionCode'] = attrs.get((ANDROID_NS, 'versionCode'))
            result['versionName'] = attrs.get((ANDROID_NS, 'versionName'))
        if name == 'uses-sdk' and depth == 1:
            result['minSdkVersion'] = attrs.get((ANDROID_NS, 'minSdkVersion'))
        if name == 'application' and depth == 1: applications += 1
        if name == 'provider':
            result['providers'].append({key: attrs.get((ANDROID_NS, key)) for key in ATTR_IDS})
        if name == 'activity':
            result['activities'].append({key: attrs.get((ANDROID_NS, key)) for key in ('name', 'exported')})
    if manifests != 1 or not isinstance(result['package'], str) or not result['package']:
        raise ValueError('Missing manifest package')
    result['applications'] = applications
    return result


def patch_manifest(data):
    """Append unexported BootstrapProvider and LogActivity declarations.

    Rejects split manifests, existing provider/authority conflicts and malformed
    input. A preexisting LogActivity is retained only when explicitly unexported
    and without intent filters. Does not rename the package or change originals.
    """
    info = inspect_manifest(data)
    if info['split']: raise ValueError('A base APK is required; split manifest supplied')
    if info['applications'] != 1: raise ValueError('Expected exactly one application')
    authority = info['package']+'.pgrazor.bootstrap'
    def qualified(name):
        if not isinstance(name, str): return name
        if name.startswith('.'): return info['package']+name
        return info['package']+'.'+name if '.' not in name else name
    for provider in info['providers']:
        name = qualified(provider['name'])
        if name == PROVIDER or authority in str(provider['authorities'] or '').split(';'):
            raise ValueError('Bootstrap provider or authority already exists')
    header_size, chunks, pool, maps, resources = _parse(data)
    existing_log_activity = False
    current_activity = None
    for _, event, name, attrs, depth in _walk(chunks, pool):
        if event == 'start' and name == 'activity':
            current_activity = {'name': attrs.get((ANDROID_NS, 'name')),
                                'exported': attrs.get((ANDROID_NS, 'exported')),
                                'depth': depth, 'filter': False}
        elif event == 'start' and name == 'intent-filter' and current_activity is not None:
            current_activity['filter'] = True
        if event != 'end' or name != 'activity' or current_activity is None: continue
        activity = current_activity
        current_activity = None
        name = qualified(activity['name'])
        if name != LOG_ACTIVITY: continue
        if existing_log_activity or activity['exported'] is not False or activity['filter'] or activity['depth'] != 2:
            raise ValueError('Conflicting log activity; it must be unexported with no intent-filter')
        existing_log_activity = True
    ends = [i for i, event, name, _, depth in _walk(chunks, pool)
            if event == 'end' and name == 'application' and depth == 1]
    if len(ends) != 1: raise ValueError('Application end not found')
    insert = ends[0]
    ns = pool.add(ANDROID_NS)
    prefix = pool.add('android')
    element = pool.add('provider')
    activity_element = pool.add('activity')
    activity_name = pool.add(LOG_ACTIVITY)
    values = {'name': pool.add(PROVIDER), 'authorities': pool.add(authority)}
    names = {}
    for key, resource_id in ATTR_IDS.items():
        names[key] = next((i for i, value in enumerate(pool.strings)
                           if value == key and i < len(resources) and resources[i] == resource_id), None)
        if names[key] is None:
            names[key] = len(pool.strings)
            pool.strings.append(key)
            resources.extend([0]*(names[key]+1-len(resources)))
            resources[names[key]] = resource_id
    line = _u32(chunks[insert][1], 8)
    def node(kind, payload):
        return struct.pack('<HHIII', kind, 16, 16+len(payload), line, NO_INDEX)+payload
    attributes = b''
    for key in sorted(ATTR_IDS, key=ATTR_IDS.get):
        value = values.get(key, NO_INDEX)
        attributes += struct.pack('<IIIHBBI', ns, names[key], value, 8, 0,
                                  0x12 if key == 'exported' else 3, 0 if key == 'exported' else value)
    start = node(0x102, struct.pack('<IIHHHHHH', NO_INDEX, element, 20, 20, 3, 0, 0, 0)+attributes)
    end = node(0x103, struct.pack('<II', NO_INDEX, element))
    # Scope our namespace locally; this works even without an existing android
    # declaration, and does not modify or depend on original namespace nesting.
    additions = [node(0x100, struct.pack('<II', prefix, ns)), start, end,
                 node(0x101, struct.pack('<II', prefix, ns))]
    activity_attributes = struct.pack('<IIIHBBI', ns, names['name'], activity_name, 8, 0, 3, activity_name)
    activity_attributes += struct.pack('<IIIHBBI', ns, names['exported'], NO_INDEX, 8, 0, 0x12, 0)
    activity_start = node(0x102, struct.pack('<IIHHHHHH', NO_INDEX, activity_element, 20, 20, 2, 0, 0, 0)+activity_attributes)
    if not existing_log_activity:
        additions[-1:-1] = [activity_start, node(0x103, struct.pack('<II', NO_INDEX, activity_element))]
    map_bytes = struct.pack('<HHI', 0x180, 8, 8+4*len(resources))+struct.pack('<'+'I'*len(resources), *resources)
    output = []
    for i, (kind, raw) in enumerate(chunks):
        if i == insert: output.extend(additions)
        if i == 0:
            output.append(pool.build())
            if not maps: output.append(map_bytes)
        elif kind == 0x180: output.append(map_bytes)
        else: output.append(raw)
    outer = bytearray(data[:header_size])
    body = b''.join(output)
    struct.pack_into('<I', outer, 4, len(outer)+len(body))
    result = bytes(outer)+body
    verified = inspect_manifest(result)
    if verified['providers'][-1] != {'name': PROVIDER, 'exported': False, 'authorities': authority}:
        raise ValueError('Provider verification failed')
    if not any(qualified(activity['name']) == LOG_ACTIVITY and activity['exported'] is False
               for activity in verified['activities']):
        raise ValueError('Activity verification failed')
    return result


def add_account_restart_activity(data):
    """Append an unexported AccountRestartActivity on a private :pgrazor_restart process.

    The helper performs cold-process account switches in isolation, so it is declared
    exported="false" with android:process=":pgrazor_restart" and no intent-filter. A
    conflicting preexisting activity of the same name (exported, wrongly scoped/missing
    process, or carrying an intent-filter) is rejected; an exactly-matching safe definition
    is accepted and the manifest returned unchanged. Rejects split manifests. Append-only:
    existing components, strings and resource ids are preserved.
    """
    info = inspect_manifest(data)
    if info['split']: raise ValueError('A base APK is required; split manifest supplied')
    if info['applications'] != 1: raise ValueError('Expected exactly one application')
    def qualified(name):
        if not isinstance(name, str): return name
        if name.startswith('.'): return info['package']+name
        return info['package']+'.'+name if '.' not in name else name
    header_size, chunks, pool, maps, resources = _parse(data)
    existing = False
    current = None
    for _, event, name, attrs, depth in _walk(chunks, pool):
        if event == 'start' and name == 'activity':
            current = {'name': attrs.get((ANDROID_NS, 'name')),
                       'exported': attrs.get((ANDROID_NS, 'exported')),
                       'process': attrs.get((ANDROID_NS, 'process')),
                       'depth': depth, 'filter': False}
        elif event == 'start' and name == 'intent-filter' and current is not None:
            current['filter'] = True
        if event != 'end' or name != 'activity' or current is None: continue
        activity = current
        current = None
        if qualified(activity['name']) != RESTART_ACTIVITY: continue
        if (existing or activity['exported'] is not False or activity['filter'] or activity['depth'] != 2
                or activity['process'] != RESTART_PROCESS):
            raise ValueError('Conflicting account restart activity; it must be unexported on '
                             + RESTART_PROCESS + ' with no intent-filter')
        existing = True
    if existing:
        # An exactly-matching safe definition is already present (e.g. our preview manifest); leave it.
        return data
    ends = [i for i, event, name, _, depth in _walk(chunks, pool)
            if event == 'end' and name == 'application' and depth == 1]
    if len(ends) != 1: raise ValueError('Application end not found')
    insert = ends[0]
    ns = pool.add(ANDROID_NS)
    prefix = pool.add('android')
    activity_element = pool.add('activity')
    activity_name = pool.add(RESTART_ACTIVITY)
    process_value = pool.add(RESTART_PROCESS)
    names = {}
    for key, resource_id in RESTART_ATTR_IDS.items():
        names[key] = next((i for i, value in enumerate(pool.strings)
                           if value == key and i < len(resources) and resources[i] == resource_id), None)
        if names[key] is None:
            names[key] = len(pool.strings)
            pool.strings.append(key)
            resources.extend([0]*(names[key]+1-len(resources)))
            resources[names[key]] = resource_id
    line = _u32(chunks[insert][1], 8)
    def node(kind, payload):
        return struct.pack('<HHIII', kind, 16, 16+len(payload), line, NO_INDEX)+payload
    attributes = struct.pack('<IIIHBBI', ns, names['name'], activity_name, 8, 0, 3, activity_name)
    attributes += struct.pack('<IIIHBBI', ns, names['exported'], NO_INDEX, 8, 0, 0x12, 0)
    attributes += struct.pack('<IIIHBBI', ns, names['process'], process_value, 8, 0, 3, process_value)
    activity_start = node(0x102, struct.pack('<IIHHHHHH', NO_INDEX, activity_element, 20, 20, 3, 0, 0, 0)+attributes)
    # Scope our namespace locally so this works with or without an existing android declaration.
    additions = [node(0x100, struct.pack('<II', prefix, ns)), activity_start,
                 node(0x103, struct.pack('<II', NO_INDEX, activity_element)),
                 node(0x101, struct.pack('<II', prefix, ns))]
    map_bytes = struct.pack('<HHI', 0x180, 8, 8+4*len(resources))+struct.pack('<'+'I'*len(resources), *resources)
    output = []
    for i, (kind, raw) in enumerate(chunks):
        if i == insert: output.extend(additions)
        if i == 0:
            output.append(pool.build())
            if not maps: output.append(map_bytes)
        elif kind == 0x180: output.append(map_bytes)
        else: output.append(raw)
    outer = bytearray(data[:header_size])
    body = b''.join(output)
    struct.pack_into('<I', outer, 4, len(outer)+len(body))
    result = bytes(outer)+body
    inspect_manifest(result)
    _, verified_chunks, verified_pool, _, _ = _parse(result)
    confirmed = False
    for _, event, name, attrs, depth in _walk(verified_chunks, verified_pool):
        if (event == 'start' and name == 'activity' and depth == 2
                and qualified(attrs.get((ANDROID_NS, 'name'))) == RESTART_ACTIVITY
                and attrs.get((ANDROID_NS, 'exported')) is False
                and attrs.get((ANDROID_NS, 'process')) == RESTART_PROCESS):
            confirmed = True
    if not confirmed: raise ValueError('Account restart activity verification failed')
    return result
