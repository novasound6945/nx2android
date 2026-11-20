# porter apk-update: a released port's APK again with this checkout's host, without the game's files and without its data pack changing (python -m porter apk-update …).
# The data pack (mods/, RomFS, shaders.dmsv, ...) stays on the server as it is; the APK keeps every entry it has (the package, the label, the icon, the gamepad theme, the
# font, the data server's address nx2a/source.bin, the Vulkan driver and its hooks, classes.dex), and gets libdmhost.so built from this checkout's profile, a new versionCode and
# versionName (the game's version and the build number: 1.0.1-8) in its manifest (axml.py: the rest of the manifest's bytes kept), signed again with the player's key, which must
# be the APK's signer (it installs over the released app and keeps its saves). Nothing is uploaded or installed.
# The pack must fit the new host (judge):
#   - the rewritten code's contract (dm/host/rewrite_abi.h): a pack's REWRITE_ABI (nx2a-data.json rewrite_abi, each fixups table's # REWRITE_ABI) must be this checkout's. A pack
#     from before the number was written is judged by its mods/build_src.sha256: the rewriter's versions that made it are looked up in git and every later code change of the
#     rewriter must be format-neutral (dm/tools/rewrite_abi.py COMMITS); anything not found or not judged counts as a different format.
#   - the profile: the host is built from this checkout's profile, so nx2a-data.json profile_h (the game_profile.h the released host was built from) must be this checkout's
#     header, or one in the profile's git history that differs from it only in defines the data pack does not depend on (HOST_ONLY: memory layout, host behaviour). Then the pack's
#     nx2a-data.json is written again with the new profile_h, with its files.json (the app checks the files by it), for the server; a define the data depends on (the modules,
#     the builds, shader keys) refuses. The pack's version must be one the profile registers.
#   - the patch files (sdk_patches.py, the games' patches.py): the cuts and observation gates this checkout gives each module's build are compared with its .cuts.tsv / .obs.tsv.
#     A difference does not refuse (it is not the contract: the pack keeps the code it was rewritten with), it is reported: a port again takes the new patches.
# --check prints the judgment only (no APK, no build): "same", "different" or "unknown" with its reasons; the update refuses all but "same".
import base64, hashlib, json, os, re, shutil, subprocess, sys, tempfile, urllib.error, urllib.parse, urllib.request, zipfile
from pathlib import Path

from porter import datapack, host, paths, publish
from porter.paths import DM
from porter.upstream import BuildError, say
import rewrite_abi                                   # noqa: E402 (dm/tools: porter.upstream puts it on sys.path)

if str(DM / 'android/apk') not in sys.path: sys.path.insert(0, str(DM / 'android/apk'))
import apkdriver, apksign, axml                      # noqa: E402

LIB = 'lib/arm64-v8a/libdmhost.so'


# ---- the data pack: a folder (<App>-data) or its folder on a web server ----
class Pack:
    """The few files of a data pack the judgment reads (nx2a-data.json, files.json, the mods tables), from a folder or over HTTP(S) with Basic authentication"""
    def __init__(self, where, cred=''):
        self.where = str(where); self.url = self.where.lower().startswith(('http://', 'https://'))
        if self.url: self.where = publish.check_url(self.where)
        elif not Path(self.where).is_dir(): raise BuildError(f'{where}: no such folder (give the data pack\'s folder or its http(s):// address)')
        self.auth = 'Basic ' + base64.b64encode(cred.encode()).decode() if cred else ''   # (never printed)
        self.cache = {}

    def __str__(self): return self.where

    def read(self, rel):   # the file's bytes, None when the pack has none
        if rel in self.cache: return self.cache[rel]
        if self.url:
            req = urllib.request.Request(self.where + urllib.parse.quote(rel), headers={'Authorization': self.auth} if self.auth else {})
            try:
                with urllib.request.urlopen(req, timeout=120) as r: b = r.read()
            except urllib.error.HTTPError as e:
                if e.code == 404: b = None
                elif e.code in (401, 403): raise BuildError(f'{self.where}: the server refused the credentials (HTTP {e.code}): give --data-cred FILE (user:password)') from None
                else: raise BuildError(f'{self.where}{rel}: HTTP {e.code}') from None
            except (urllib.error.URLError, OSError) as e: raise BuildError(f'{self.where}: the server cannot be reached ({getattr(e, "reason", e)})') from None
        else:
            p = Path(self.where) / rel; b = p.read_bytes() if p.is_file() else None
        self.cache[rel] = b
        return b

    def text(self, rel):
        b = self.read(rel)
        return b.decode('utf-8', 'replace') if b is not None else None

    def json(self, rel):
        t = self.text(rel)
        try: return json.loads(t) if t is not None else None
        except ValueError: raise BuildError(f'{self.where}{rel}: not JSON') from None

    def names(self):   # the paths of the pack's loose files under mods/ and mods-<variant>/: files.json's list on a server (it lists no folders), the folder's own
        if self.url:
            fj = self.json(publish.FILES)
            if not isinstance(fj, dict): raise BuildError(f'{self.where}: no {publish.FILES} (the list of the pack\'s files, porter publish)')
            return [f['path'] for f in fj.get('files', []) if 'bundle' not in f]
        root = Path(self.where)
        return [p.relative_to(root).as_posix() for d in sorted(root.iterdir()) if d.is_dir() and (d.name == 'mods' or d.name.startswith(datapack.VARIANT_PREFIX))
                for p in sorted(d.iterdir()) if p.is_file()]


def pack_modules(pack):
    """{mods folder: [module_tables() of each module, with 'name' and 'abi' (# REWRITE_ABI of its fixups table, None without)]}"""
    out = {}
    for rel in pack.names():
        d, _, n = rel.rpartition('/')
        if (d == 'mods' or d.startswith(datapack.VARIANT_PREFIX)) and n.endswith('.cuts.tsv'):
            m = n[:-len('.cuts.tsv')]
            t = host.module_tables(pack.text(f'{d}/{m}.cuts.tsv') or '', pack.text(f'{d}/{m}.obs.tsv') or '')
            fx = pack.text(f'{d}/{m}.fixups.tsv')
            out.setdefault(d, []).append(dict(t, name=m, abi=rewrite_abi.from_fixups(fx) if fx is not None else None))
    return out


# ---- the profile: the header the released host was built from, found in git, and what changed ----
# game_profile.h defines the data pack does not depend on: the host's memory layout and behaviour. A port's data (the rewritten modules, the RomFS, the shaders) is the same
# whatever they are; the host built from this checkout's profile takes the new values. Any other define (the modules, the builds, the RomFS marker, the program ID, shader key
# lists), and any define not named here, refuses.
HOST_ONLY = frozenset(f'DM_GAME_{k}' for k in (
    'NAME', 'MEM_RESERVE', 'MEM_ALIAS', 'MEM_STACK', 'APP_POOL', 'MEM_USED', 'SYS_RESOURCE', 'ARGS', 'ENV', 'MEM_MAP_LIMIT', 'SMALL_VPOOL_SPARSE', 'PRESENT_DISPLAY',
    'CORE_PREEMPT', 'ALLOC_BIG_ALIGN', 'FPS_TARGET', 'FPS_CEILING', 'FPS_HOLD', 'SHARED_FONT_TTF', 'PAD_LAYOUT', 'OPMODE', 'PERF_BOOST', 'TEX_FLUSH_UPLOAD', 'RENDER_COND',
    'TEX_DIRTY_UPLOAD', 'TEX_DYN_SLOTS', 'TEX_CPU_UNCACHED', 'GPU_TS_SEGMENT', 'VCONV_RANGE', 'ORDER_KEEP', 'NVN_API_MAJOR', 'NVN_API_MINOR',
    'SOCK_LOOPBACK'))   # SOCK_LOOPBACK (upstream f6cd25ef): the host's offline sockets on the loopback interface
VERSIONS = 'DM_GAME_DISPLAY_VERSIONS'   # the builds: a version added is host-only, one changed or gone refuses


def defines(header):   # {name: value} of a game_profile.h's #define lines
    out = {}
    for ln in header.decode('utf-8', 'replace').splitlines():
        m = re.match(r'#define (\w+)(?: (.*))?$', ln.strip())
        if m: out[m.group(1)] = (m.group(2) or '').strip()
    return out


def profile_change(old, new):
    """(host-only changes, data changes): the define names that differ between two game_profile.h (bytes)"""
    a, b = defines(old), defines(new); hosted, data = [], []
    for k in sorted(set(a) | set(b)):
        if a.get(k) == b.get(k): continue
        if k == VERSIONS and k in a and k in b:
            entries = lambda v: set(re.findall(r'\{"([^"]*)", "([^"]*)"\}', v))
            (hosted if entries(a[k]) <= entries(b[k]) else data).append(k)
        else: (hosted if k in HOST_ONLY else data).append(k)
    return hosted, data


def header_history(title_id):
    """[(commit, game_profile.h bytes)] of title_id's profile at each commit that changed its game.tsv or game.py (newest first), made by that commit's game.py"""
    games = DM / 'games'
    out = subprocess.run(['git', 'log', '--full-history', '--format=%H', '--', f'{title_id}/game.tsv', 'game.py'], cwd=games, capture_output=True, text=True)
    if out.returncode: return []
    seen, rows = {}, []
    with tempfile.TemporaryDirectory(prefix='nx2a_profile_') as t:
        for c in out.stdout.split():
            tsv, py = rewrite_abi.blob(c, f'{title_id}/game.tsv', games), rewrite_abi.blob(c, 'game.py', games)
            if tsv is None or py is None: continue
            key = hashlib.sha256(tsv + b'\0' + py).hexdigest()
            if key not in seen:
                d = Path(t) / key[:16]; (d / 'games' / title_id).mkdir(parents=True)
                (d / 'games/game.py').write_bytes(py); (d / 'games' / title_id / 'game.tsv').write_bytes(tsv)
                r = subprocess.run([sys.executable, '-I', str(d / 'games/game.py'), 'header', title_id, str(d / 'h')], capture_output=True, env=host.clean_env())
                seen[key] = (d / 'h').read_bytes() if r.returncode == 0 and (d / 'h').is_file() else None
            if seen[key] is not None: rows.append((c, seen[key]))
    return rows


def derived_dir(title_id):   # where the derived profile of an unregistered version's port is made: a fixed folder, so its header (which names it) is the same at every update
    return paths.user_dir() / 'apk-update' / 'profile-derived' / title_id


def derive(title_id, man, modules, old=None):
    """The profile a port of man's version (one the profile does not register) is built from, made again from this checkout's profile in derived_dir (as
    build_ipa_from_nsp.py derive_profile and porter's alias_headroom make it): its build row from the pack's modules' build IDs, its package and shader versions, the alias
    region's headroom (the released header old's, else the automatic 8 GB) and a mem_used the rewrite refined (old's). → its game_profile.h (bytes)"""
    from porter import pipeline
    src, d = DM / 'games' / title_id, derived_dir(title_id)
    if d.exists(): shutil.rmtree(d)
    d.mkdir(parents=True)
    for f in src.iterdir():
        if f.is_file() and f.name not in ('game.tsv', 'patches.py'): shutil.copyfile(f, d / f.name)
    sys.path.insert(0, str(DM / 'games'))
    try: import game
    finally: sys.path.pop(0)
    g = game.Game(src); ver, T = man['version'], '\t'
    bids = {m['name']: m['build_id'] for m in modules.get('mods', [])}
    row = T.join(['build', ver, man.get('display', '?')] + [f'{m}={bids[m]}' for m in g.words('modules') if bids.get(m)])
    lines, put = [], False
    for ln in (src / 'game.tsv').read_text(encoding='utf-8').splitlines():
        if ln.split(T, 1)[0] in ('build', 'package', 'shader_versions'):
            if not put: lines += [row, f'package{T}{ver}', f'shader_versions{T}{ver}']; put = True
            continue
        lines.append(ln)
    od = defines(old) if old else {}
    if old and 'mem_used' not in {ln.split(T, 1)[0] for ln in lines} and int(od.get('DM_GAME_MEM_USED', '0x4000000').rstrip('ul'), 16) != 0x4000000:
        lines.append(f'mem_used{T}{od["DM_GAME_MEM_USED"].rstrip("ul")}')   # (the rewrite's refine: modules larger than the default 64 MB of used memory)
    (d / 'game.tsv').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    gb = None   # automatic
    if old:
        try:
            add = int(od['DM_GAME_MEM_ALIAS'].rstrip('ul'), 16) - g.int('mem_alias')
            if add >= 0 and add % (1 << 30) == 0 and add == int(od['DM_GAME_MEM_RESERVE'].rstrip('ul'), 16) - g.int('mem_reserve'): gb = add >> 30
        except (KeyError, ValueError): pass
    pipeline.alias_headroom(d / 'game.tsv', gb)
    return host.profile_header(title_id, games_dir=d.parent)


def judge_profile(man, title_id, modules=None, old_header=None):
    """{'verdict': 'same' | 'host-only' | 'different' | 'unknown', 'reasons', 'profile_h' (of the header the new host is built from), 'changed' (host-only define names),
    'games_dir' (the derived profile's folder, None for the repository's)}. old_header: the released host's game_profile.h, when git does not have it (a derived profile's)"""
    sys.path.insert(0, str(DM / 'games'))
    try: import game
    finally: sys.path.pop(0)
    out = {'reasons': [], 'changed': [], 'games_dir': None}
    builds = game.load(title_id).builds(); ver = man.get('version', '')
    ph = (man.get('profile_h') or '').lower()
    if old_header is not None and ph not in rewrite_abi.sha256s(old_header):
        return dict(out, verdict='different', profile_h='', reasons=[f'--profile-header: its SHA-256 is not the pack\'s profile_h {ph[:12] or "(none)"}'])
    def compare(old, cur, where):
        hosted, data = profile_change(old, cur)
        if data: return dict(out, verdict='different', changed=hosted, reasons=[f'{where} differs from the header made now in defines the data pack depends on: {", ".join(data)}'])
        return dict(out, verdict='host-only' if hashlib.sha256(cur).hexdigest() not in rewrite_abi.sha256s(old) else 'same', changed=hosted,
                    reasons=[f'{where}; the header made now differs in host-only defines: {", ".join(hosted) or "(none: comments only)"}'])
    if ver not in builds:   # a derived profile's port: the profile made again for its version
        if modules is None: return dict(out, verdict='unknown', profile_h='', reasons=[f'version {ver} is not one the profile registers, and no modules to derive its profile from'])
        cur = derive(title_id, man, modules, old_header)
        out.update(profile_h=hashlib.sha256(cur).hexdigest(), games_dir=str(derived_dir(title_id).parent))
        note = f'version {ver} is not one the profile registers ({", ".join(builds) or "none"}): a derived profile\'s port, made again in {derived_dir(title_id)}'
        if ph and ph in rewrite_abi.sha256s(cur): return dict(out, verdict='same', reasons=[note, 'profile_h is the derived header made now'])
        if old_header is None:
            return dict(out, verdict='unknown', reasons=[note, 'profile_h is not the derived header made now (its first line names the folder it was made in): give the released '
                                                         'host\'s header, --profile-header <its build folder>/android/host/gen/<title ID>/game_profile.h'])
        r = compare(old_header, cur, 'the released derived header (--profile-header)')
        r['reasons'].insert(0, note)
        return r
    cur = host.profile_header(title_id); out['profile_h'] = hashlib.sha256(cur).hexdigest()
    if not ph: return dict(out, verdict='unknown', reasons=['no profile_h in nx2a-data.json (a pack from before porter recorded the host\'s profile)'])
    if ph in rewrite_abi.sha256s(cur): return dict(out, verdict='same', reasons=['profile_h is this checkout\'s'])
    for hdr in (cur, cur.replace(b'\n', b'\r\n')):   # (a pack made on Windows hashed the header with CRLF line ends)
        for names, h in host.older_hashes(hdr):
            if ph == h: return dict(out, verdict='same', reasons=[f'profile_h is this checkout\'s without {", ".join(names)} (added since, off)'])
    if old_header is not None: return compare(old_header, cur, 'the released header (--profile-header)')
    for c, old in header_history(title_id):
        if ph in rewrite_abi.sha256s(old): return compare(old, cur, f'the profile of {c[:8]}')
    return dict(out, verdict='unknown', reasons=[f'profile_h {ph[:12]} is no header of the profile in this checkout\'s git history (--alias-headroom, a provisional profile, or '
                                                 f'uncommitted changes): give the released host\'s header with --profile-header if it is at hand'])


# ---- the rewritten code ----
def judge_rewrite(man, modules, record):
    """{'verdict': 'same' | 'different' | 'unknown', 'abi' (the pack's, None for a pack from before the number), 'reasons'}"""
    want = rewrite_abi.current()
    got = {m['abi'] for ms in modules.values() for m in ms} | ({man['rewrite_abi']} if man.get('rewrite_abi') is not None else set())
    if got - {None}:
        if len(got) > 1: return {'verdict': 'different', 'abi': None, 'reasons': [f'the pack\'s modules carry different REWRITE_ABI numbers ({", ".join(sorted(map(str, got)))})']}
        abi = got.pop()
        if abi == want: return {'verdict': 'same', 'abi': abi, 'reasons': [f'REWRITE_ABI {abi}, this checkout\'s']}
        return {'verdict': 'different', 'abi': abi, 'reasons': [f'REWRITE_ABI {abi}, this checkout\'s is {want}: port the game again from its files']}
    if not record or not record.strip(): return {'verdict': 'unknown', 'abi': None, 'reasons': ['no REWRITE_ABI and no mods/build_src.sha256']}
    core = {Path(rel).name: h for h, rel in host.record_rows(record) if '/' not in rel and rel in rewrite_abi.CORE}
    verdict, reasons = rewrite_abi.legacy_verdict(core)
    reasons = ['no REWRITE_ABI (a pack from before the number): judged by the rewriter\'s git history'] + reasons
    if verdict == 'same' and rewrite_abi.LEGACY != want:
        verdict = 'different'; reasons.append(f'the rewriter of the pack is contract {rewrite_abi.LEGACY}, this checkout\'s is {want}')
    return {'verdict': verdict, 'abi': None, 'reasons': reasons}


def _patch_tables(variant):   # [(file, {'CUTS', 'OBS', 'FRAME_STEP'})] of every patch file patch_nso.py loads, in its order, as they read with DM_PATCH_VARIANT=variant
    import importlib.util
    files = [DM / 'tools/sdk_patches.py'] + sorted((DM / 'games').glob('*/patches.py'))
    old = os.environ.get('DM_PATCH_VARIANT')
    if variant: os.environ['DM_PATCH_VARIANT'] = variant
    else: os.environ.pop('DM_PATCH_VARIANT', None)
    try:
        out = []
        for n, f in enumerate(files):
            spec = importlib.util.spec_from_file_location(f'nx2a_apkupdate_patches_{n}', f); m = importlib.util.module_from_spec(spec)
            try: spec.loader.exec_module(m)
            except Exception as e: raise BuildError(f'{f}: does not load ({type(e).__name__}: {e})') from None
            out.append((f, {k: getattr(m, k, {}) for k in ('CUTS', 'OBS', 'FRAME_STEP')}))
        return out
    finally:
        if old is None: os.environ.pop('DM_PATCH_VARIANT', None)
        else: os.environ['DM_PATCH_VARIANT'] = old


def judge_patches(modules, record):
    """[notes]: where this checkout's patch files give a module's build other cuts or observation gates than it was rewritten with, or another frame step"""
    rows = {rel: h for h, rel in host.record_rows(record or '')}
    extra = any(host.outside(rel) for rel in rows)   # a --fps60 port: its own cuts after the registered ones
    notes = []
    for d, ms in sorted(modules.items()):
        tables = _patch_tables(d[len(datapack.VARIANT_PREFIX):] if d.startswith(datapack.VARIANT_PREFIX) else '')
        for m in ms:
            bid = m['build_id']
            if not bid: notes.append(f'{d}/{m["name"]}: no build ID in its .cuts.tsv'); continue
            cuts = [(o, ob.lower(), nb.lower()) for _, t in tables for o, ob, nb, _ in t['CUTS'].get(bid, [])]
            obs = [(o, str(n).lower(), str(w).lower()) for _, t in tables for o, n, w, _ in t['OBS'].get(bid, [])]
            stepped = any(bid in t['FRAME_STEP'] for _, t in tables)   # its frame step's writes are listed after the registered cuts (patch_nso.py fs_put); the step itself is checked below
            if m['cuts'] != cuts and not ((extra or stepped) and m['cuts'][:len(cuts)] == cuts):
                notes.append(f'{d}/{m["name"]} (build {bid[:16]}): {len(m["cuts"])} cut(s) in the pack, {len(cuts)} from this checkout\'s patch files')
            if m['obs'] != obs: notes.append(f'{d}/{m["name"]} (build {bid[:16]}): {len(m["obs"])} observation gate(s) in the pack, {len(obs)} from this checkout\'s patch files')
            for f, t in tables:
                if bid not in t['FRAME_STEP']: continue
                rel = Path(os.path.relpath(f, DM / 'tools')).as_posix(); h = rows.get(rel)
                now = f.read_bytes()
                if h and h in rewrite_abi.sha256s(now): continue
                found = rewrite_abi.find_version(rel, h, DM / 'tools') if h else []
                if found and rewrite_abi.code_tokens(rewrite_abi.blob(found[0], rel, DM / 'tools')) == rewrite_abi.code_tokens(now): continue
                notes.append(f'{d}/{m["name"]} (build {bid[:16]}): its frame step ({rel}) ' + ('is not in the pack\'s record' if not h else 'changed since the pack'))
    return notes


def judge(pack, old_header=None):
    """The pack's fit with this checkout's host (old_header: the released host's game_profile.h, judge_profile): {'title_id', 'package', 'version', 'display', 'verdict' (same / different / unknown), 'ok', 'rewrite', 'profile', 'patches', 'manifest'}"""
    man = pack.json(datapack.MANIFEST)
    if not isinstance(man, dict): raise BuildError(f'{pack}: no {datapack.MANIFEST} (not a data pack)')
    tid = man.get('title_id', '')
    if not isinstance(tid, str) or not host.TITLE_ID.fullmatch(tid): raise BuildError(f'{pack}{datapack.MANIFEST}: no title ID (16 hexadecimal digits): {tid!r}')
    tid = tid.upper()
    if not (DM / 'games' / tid / 'game.tsv').is_file(): raise BuildError(f'{tid}: no profile of the game in this checkout (an experimental port\'s profile stays in its build folder)')
    record = pack.text('mods/build_src.sha256') or ''
    modules = pack_modules(pack)
    if not modules.get('mods'): raise BuildError(f'{pack}: no rewritten modules (mods/<module>.cuts.tsv) in the pack')
    rw, pr, notes = judge_rewrite(man, modules, record), judge_profile(man, tid, modules, old_header), judge_patches(modules, record)
    order = ('same', 'unknown', 'different')
    verdict = max(rw['verdict'], 'same' if pr['verdict'] in ('same', 'host-only') else pr['verdict'], key=order.index)
    return {'title_id': tid, 'package': man.get('package', ''), 'version': man.get('version', ''), 'display': man.get('display', ''), 'verdict': verdict, 'ok': verdict == 'same',
            'rewrite': rw, 'profile': pr, 'patches': notes, 'manifest': man}


def report_lines(r):   # the judgment as the command prints it
    out = [f'{r["title_id"]} {r["package"]} {r["version"]} ({r["display"]}): {r["verdict"].upper()}' + ('' if r['ok'] else ' (porter apk-update refuses it: port the game again from its files)')]
    out += [f'  rewrite ({r["rewrite"]["verdict"]}): {x}' for x in r['rewrite']['reasons']]
    out += [f'  profile ({r["profile"]["verdict"]}): {x}' for x in r['profile']['reasons']]
    out += [f'  patches (not refused, the pack keeps its code): {x}' for x in r['patches']]
    return out


# ---- the APK ----
def source_of(apk):
    """(url, user:password) of an APK's nx2a/source.bin (publish.source_asset), (None, '') without one. The credentials are never printed"""
    with zipfile.ZipFile(apk) as z:
        try: b = z.read('assets/' + publish.SOURCE_ASSET)
        except KeyError: return None, ''
    if b[:4] != b'NX2S' or b[4] not in (1, 2): raise BuildError(f'{apk}: assets/{publish.SOURCE_ASSET} is not a data source asset')
    rows = publish.mask(b[5:]).decode('utf-8', 'replace').split('\n')   # (the mask is its own inverse)
    return rows[0], rows[1] if len(rows) > 1 else ''


def next_build(package, apk_offset):   # the build number by default: one more than the APK's and than this computer's last build of the package (builds.json)
    try: seen = json.loads((paths.user_dir() / 'builds.json').read_text(encoding='utf-8'))
    except (OSError, ValueError): seen = {}
    return max(apk_offset, int(seen.get(package, 0))) + 1


def update(apk, pack, out, build=None, jobs=os.cpu_count() or 8, old_header=None):
    """apk (a released port's APK file) with this checkout's host and build number build, judged against its data pack pack, written into the folder out (with a new
    nx2a-data.json and files.json when the profile's hash changed). → {'apk', 'package', 'version_code', 'version_name', 'host', 'data_files', 'judgment'}"""
    from porter import pipeline
    apk, out = Path(apk), Path(out)
    old = axml.apk_info(apk)
    r = judge(pack, old_header)
    if old['package'] != r['package']: raise BuildError(f'{apk}: package {old["package"]}, the data pack\'s is {r["package"]}: not this game\'s pack')
    for ln in report_lines(r): say(ln)
    if not r['ok']: raise BuildError(f'{r["package"]}: the data pack does not fit this checkout\'s host ({r["verdict"]}): port the game again from its files')
    man = r['manifest']
    base = pipeline.version_code(man['version'], 0) - 1   # (the Switch version number)
    offset = next_build(old['package'], old['version_code'] - base - 1) if build is None else build
    code = pipeline.version_code(man['version'], offset)   # (an offset out of range refuses here)
    name = f'{man["display"]}-{offset}' if offset else man['display']
    if code <= old['version_code']:
        raise BuildError(f'versionCode {code} (build {offset}) is not above the APK\'s {old["version_code"]} ({old["version_name"]}): Android installs only a higher one over it; give a higher --build')
    key = paths.signing_key()
    if not key.is_file(): raise BuildError(f'no signing key {key}: the APK must be signed with the key it was released with')
    kj = json.loads(key.read_text()); crt = bytes.fromhex(kj['cert'])
    if [hashlib.sha256(crt).hexdigest()] != old['signers']:
        raise BuildError(f'{apk} was signed with another key than {key}: an APK signed with this one would not install over the released app')
    dst = out / apk.name
    if dst.resolve() == apk.resolve(): raise BuildError(f'-o {out}: the APK would be written over itself; give another folder')
    out.mkdir(parents=True, exist_ok=True)
    cache = paths.user_dir() / 'apk-update' / (r['title_id'] + ('-debug' if old['debuggable'] else ''))   # the host's objects, kept between updates (build.py rebuilds what changed)
    cache.mkdir(parents=True, exist_ok=True)
    so = host.build_host(old['package'], r['title_id'], old['debuggable'], cache, jobs, r['profile']['games_dir'])
    with zipfile.ZipFile(apk) as z: have = set(z.namelist()); manifest = z.read('AndroidManifest.xml')
    fonts = {n: f for n, f in pipeline.ui_font_assets().items() if f'assets/{n}' not in have}   # the overlay's font, for an APK released before it came; the APK's own is kept
    entries, _, _ = apkdriver.rebuild(str(apk), apkdriver.KEEP, host=str(so), assets=fonts, manifest=axml.set_version(manifest, code, name))
    k = {n: int(v) for n, v in kj.items() if n != 'cert'}
    z, cd, eocd = apksign.build_zip(entries)
    tmp = dst.with_name(dst.name + '.partial'); tmp.write_bytes(apksign.sign_v2(z, cd, eocd, k, crt))
    new = axml.apk_info(tmp)
    if (new['package'], new['version_code'], new['version_name'], new['signers']) != (old['package'], code, name, old['signers']):
        tmp.unlink(); raise BuildError(f'the APK written does not read back as {old["package"]} {code} ({name}) with the same signer')
    os.replace(tmp, dst)
    pipeline.build_number(old['package'], offset)   # remembered: the next port or update counts on from it
    say(f'{dst}: {old["version_name"]} ({old["version_code"]}) → {name} ({code}), host {so.stat().st_size / 2**20:.1f} MB, signer {old["signers"][0][:16]}…')
    written = []
    if r['profile']['verdict'] == 'host-only':   # the released host's profile_h no longer describes the APK's: the pack's manifest again, and its list (the app checks each file by it)
        m2 = dict(man, profile_h=r['profile']['profile_h'])
        text = json.dumps(m2, indent=2).encode()
        (out / datapack.MANIFEST).write_bytes(text); written.append(datapack.MANIFEST)
        fj = pack.json(publish.FILES)
        if isinstance(fj, dict):
            (out / publish.FILES).write_text(publish.dump(publish.replace_file(fj, datapack.MANIFEST, text)), encoding='utf-8', newline='\n'); written.append(publish.FILES)
        say(f'the profile changed in host-only defines ({", ".join(r["profile"]["changed"]) or "none"}): new profile_h {r["profile"]["profile_h"][:16]}…; upload {", ".join(written)} '
            f'from {out} into the pack\'s folder ({publish.FILES} last)' + ('' if publish.FILES in written else f' (the pack has no {publish.FILES}: run porter publish on it)'))
    if r['profile']['games_dir']:
        say(f'the host was built from the derived profile {derived_dir(r["title_id"])}; its header is {cache / "gen" / r["title_id"] / "game_profile.h"}: a later update on another '
            f'computer needs it (--profile-header)')
    return {'apk': str(dst), 'package': old['package'], 'version_code': code, 'version_name': name, 'old': {k: old[k] for k in ('version_code', 'version_name')},
            'host': so.stat().st_size, 'data_files': written, 'judgment': {k: v for k, v in r.items() if k != 'manifest'}}
