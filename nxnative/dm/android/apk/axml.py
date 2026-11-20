#!/usr/bin/env python3
# axml.py: read and change an APK's binary AndroidManifest.xml (AXML, as mkapk.py writes it: one UTF-16 string pool, a resource map, the element tree), and read the signer
# certificates of its APK Signature Scheme v2 block, with the Python standard library only. porter apk-update (porter/apkupdate.py) gives a released APK a new versionCode and
# versionName this way: every other byte of the manifest stays as the APK's port wrote it (the package, the label, the activity, the permissions of that port's mkapk.py).
# Usage: axml.py <app.apk>   → prints the manifest's package, versionCode, versionName, debuggable, label and the signer certificates' SHA-256 (JSON)
import hashlib, json, struct, sys, zipfile

ANDROID_NS = 'http://schemas.android.com/apk/res/android'
T_STRING, T_INT_DEC, T_BOOL = 0x03, 0x10, 0x12
RES_STRING_POOL, RES_XML, RES_START_ELEMENT = 0x0001, 0x0003, 0x0102


def _chunks(data, start, end):   # (offset, type, header size, size) of the chunks between start and end
    o = start
    while o + 8 <= end:
        t, hs, size = struct.unpack_from('<HHI', data, o)
        if size < 8 or o + size > end: raise ValueError(f'AXML: a chunk at {o:#x} runs past its parent')
        yield o, t, hs, size
        o += size


def _pool(data, o):   # the strings of a string pool chunk at o (UTF-16 or UTF-8)
    _, hs, size, count, styles, flags, sstart, _ = struct.unpack_from('<HHIIIIII', data, o)
    if styles: raise ValueError('AXML: a string pool with styles (not one mkapk.py writes)')
    out, utf8 = [], bool(flags & 0x100)
    for i in range(count):
        p = o + sstart + struct.unpack_from('<I', data, o + hs + 4 * i)[0]
        if utf8:
            n = data[p]; p += 2 if n & 0x80 else 1   # UTF-16 length, then the UTF-8 length
            n = data[p]; p += 1
            if n & 0x80: n = (n & 0x7f) << 8 | data[p]; p += 1
            out.append(data[p:p + n].decode('utf-8'))
        else:
            n = struct.unpack_from('<H', data, p)[0]; p += 2
            if n & 0x8000: n = (n & 0x7fff) << 16 | struct.unpack_from('<H', data, p)[0]; p += 2
            out.append(data[p:p + 2 * n].decode('utf-16-le'))
    return out, utf8


def pool_chunk(strings):   # a UTF-16 string pool chunk, as mkapk.py writes it
    offs, body = [], b''
    for x in strings:
        u = x.encode('utf-16-le')
        if len(u) // 2 >= 0x8000: raise ValueError('AXML: a string too long for this writer')
        offs.append(len(body)); body += struct.pack('<H', len(u) // 2) + u + b'\0\0'
    body += b'\0' * (-len(body) % 4)
    hdr = struct.pack('<IIIII', len(strings), 0, 0, 28 + 4 * len(strings), 0)
    payload = hdr + b''.join(struct.pack('<I', o) for o in offs) + body
    return struct.pack('<HHI', RES_STRING_POOL, 28, 8 + len(payload)) + payload


def parse(data):
    """→ {'strings': [...], 'pool': (offset, size), 'elements': [(tag, {name: (offset of the attribute, raw string index, type, data)})]} of an AXML file"""
    t, hs, size = struct.unpack_from('<HHI', data, 0)
    if t != RES_XML or size != len(data): raise ValueError('not a binary XML file (AXML)')
    strings, pool, elements = None, None, []
    for o, ct, chs, csize in _chunks(data, hs, size):
        if ct == RES_STRING_POOL:
            if pool: raise ValueError('AXML: two string pools')
            strings, utf8 = _pool(data, o); pool = (o, csize)
            if utf8: raise ValueError('AXML: a UTF-8 string pool (not one mkapk.py writes)')
        elif ct == RES_START_ELEMENT:
            if strings is None: raise ValueError('AXML: an element before the string pool')
            ns, name, astart, asize, acount = struct.unpack_from('<IIHHH', data, o + chs)
            attrs = {}
            for i in range(acount):
                a = o + chs + astart + i * asize
                ans, aname, raw, _, _, dt, val = struct.unpack_from('<IIIHBBI', data, a)
                key = strings[aname] if ans == 0xffffffff else ('android:' + strings[aname] if strings[ans] == ANDROID_NS else strings[aname])
                attrs[key] = (a, raw, dt, val)
            elements.append((strings[name], attrs))
    if strings is None: raise ValueError('AXML: no string pool')
    return {'strings': strings, 'pool': pool, 'elements': elements}


def _value(strings, attr):
    _, raw, dt, val = attr
    if dt == T_STRING: return strings[val]
    if dt == T_BOOL: return val != 0
    if raw != 0xffffffff: return strings[raw]
    return val


def info(data):
    """The manifest's package, versionCode, versionName, debuggable (False without the attribute), label and activity"""
    m = parse(data); s = m['strings']
    el = {tag: attrs for tag, attrs in reversed(m['elements'])}   # the first of each tag
    man, app, act = el.get('manifest', {}), el.get('application', {}), el.get('activity', {})
    get = lambda a, k, d=None: _value(s, a[k]) if k in a else d
    return {'package': get(man, 'package'), 'version_code': get(man, 'android:versionCode'), 'version_name': get(man, 'android:versionName'),
            'debuggable': bool(get(app, 'android:debuggable', False)), 'label': get(app, 'android:label'), 'activity': get(act, 'android:name')}


def set_version(data, code, name):
    """data with the manifest's versionCode = code and versionName = name; every other byte as it was. The name is appended to the string pool (indexes of the strings before
    it, the resource map's among them, do not move) unless the pool has it already"""
    m = parse(data); strings = list(m['strings'])
    man = next((attrs for tag, attrs in m['elements'] if tag == 'manifest'), None)
    if not man or 'android:versionCode' not in man or 'android:versionName' not in man: raise ValueError('AXML: the manifest has no versionCode / versionName attribute')
    if not 0 < code < 2 ** 31: raise ValueError(f'versionCode {code}: not 1 .. 2^31-1')
    if name in strings: idx = strings.index(name)
    else: idx = len(strings); strings.append(name)
    po, psize = m['pool']; delta = 0
    new_pool = pool_chunk(strings) if len(strings) != len(m['strings']) else data[po:po + psize]
    delta = len(new_pool) - psize
    out = bytearray(data[:po] + new_pool + data[po + psize:])
    struct.pack_into('<I', out, 4, len(out))
    def at(a): return a + delta if a > po else a
    a = at(man['android:versionCode'][0]); struct.pack_into('<IHBBI', out, a + 8, 0xffffffff, 8, 0, T_INT_DEC, code)
    a = at(man['android:versionName'][0]); struct.pack_into('<IHBBI', out, a + 8, idx, 8, 0, T_STRING, idx)
    return bytes(out)


def signer_certs(apk):
    """The DER certificates of the APK Signature Scheme v2 signers of apk (bytes), [] for an APK without a v2 block"""
    eocd = apk.rfind(b'PK\x05\x06')
    if eocd < 0: raise ValueError('not a ZIP (no end of central directory)')
    cd = struct.unpack_from('<I', apk, eocd + 16)[0]
    if apk[cd - 16:cd] != b'APK Sig Block 42': return []
    size = struct.unpack_from('<Q', apk, cd - 24)[0]; start = cd - size - 8
    o, end, certs = start + 8, cd - 24, []
    def lp(b, o): n = struct.unpack_from('<I', b, o)[0]; return b[o + 4:o + 4 + n], o + 4 + n
    while o < end:
        n, pid = struct.unpack_from('<QI', apk, o); value = apk[o + 12:o + 8 + n]; o += 8 + n
        if pid != 0x7109871a: continue
        signers, _ = lp(value, 0); so = 0
        while so < len(signers):
            signer, so = lp(signers, so)
            signed, _ = lp(signer, 0)
            _, co = lp(signed, 0)          # the digests
            cl, _ = lp(signed, co); k = 0  # the certificates
            while k < len(cl): c, k = lp(cl, k); certs.append(c)
    return certs


def apk_info(path):
    """info() of an APK's manifest, with 'signers': the SHA-256 of each v2 signer certificate"""
    raw = open(path, 'rb').read()
    with zipfile.ZipFile(path) as z: out = info(z.read('AndroidManifest.xml'))
    out['signers'] = [hashlib.sha256(c).hexdigest() for c in signer_certs(raw)]
    return out


if __name__ == '__main__': print(json.dumps(apk_info(sys.argv[1]), indent=1))
