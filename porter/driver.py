# Another Vulkan driver for a port already on the device (python -m porter driver …), without the game's files: the installed APK is pulled, written again with the new
# driver library by nxnative/dm/android/apk/apkdriver.py and signed with the player's key (the same signer: it installs over the app, its saves stay), and env.txt's
# DM_VK_DRIVER row names the library (gfx_vk_core.c vk_init loads it through libadrenotools; no row: the device's driver). The game's data on the device is not touched.
# Only an APK ported with a driver carries libadrenotools: one ported without needs one port with --vulkan-driver first.
#
# The drivers folder (porter driver --push / --select / --list): a test tool beside that, not a replacement. Driver libraries put in the files folder's drivers/ and one of
# them named by DM_VK_DRIVER: the app loads that file instead of the APK's (vk_init drv_slot copies it into the app's own storage, as shared storage is noexec), from its next
# start, with no APK rebuilt. The reference stays the APK's driver (--select apk); an env.txt without such a row works as before.
import json, posixpath, re, shlex, subprocess, sys, tempfile, zipfile
from pathlib import Path

from porter import datapack, paths
from porter.adb import AdbError
from porter.paths import DM
from porter.upstream import BuildError, say

ROW = 'DM_VK_DRIVER'
DRIVERS = 'drivers'   # the files folder's folder of test drivers (gfx_vk_core.c drv_slot)
OWN = {'libdmhost.so', 'libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so'}   # as apkdriver.py OWN: not a driver
# porter install merges env.txt with the pack's (install.py merge_env), whose rows win for the keys they define: a port made with --vulkan-driver sets DM_VK_DRIVER
NOTE = 'the next porter install may put the pack\'s DM_VK_DRIVER back (the pack\'s env.txt rows win): select again after an install'


def env_with_driver(text, lib):
    """env.txt with its DM_VK_DRIVER row naming lib, in place of the old row (appended when there was none), or dropped when lib is '' (the device's driver)"""
    lines = text.splitlines(); out, done = [], False
    for ln in lines:
        if re.match(rf'\s*{ROW}\s*=', ln):
            if lib and not done: out.append(f'{ROW}={lib}'); done = True
            continue
        out.append(ln)
    if lib and not done: out.append(f'{ROW}={lib}')
    return '\n'.join(out) + '\n'


def installed_apk(adb, package):   # the installed APK's path on the device (ports are one APK, no splits)
    rows = [ln[len('package:'):].strip() for ln in adb.shell(f'pm path {shlex.quote(package)}', check=False).splitlines() if ln.startswith('package:')]
    if not rows: raise BuildError(f'{package} is not installed on the device')
    return rows[0]


def swap(adb, package, driver=None):
    """package's APK again with driver (a .so path), or with the device's own driver when None; env.txt names it. → {'package', 'driver', 'dropped'}"""
    if driver and not Path(driver).is_file(): raise BuildError(f'{driver}: no such driver library')
    with tempfile.TemporaryDirectory(prefix='nx2a_driver_') as t:
        src, out = Path(t) / 'installed.apk', Path(t) / 'driver.apk'
        say(f'{package}: pulling the installed APK')
        adb.run('pull', installed_apk(adb, package), src, timeout=600)
        r = subprocess.run([sys.executable, str(DM / 'android/apk/apkdriver.py'), str(src), str(out), str(paths.signing_key())]
                           + (['--driver', str(driver)] if driver else ['--system']), capture_output=True, text=True, errors='replace')
        if r.returncode: raise BuildError((r.stderr.strip() or r.stdout.strip()).splitlines()[-1])
        res = json.loads(r.stdout.strip().splitlines()[-1])
        adb.shell(f'am force-stop {shlex.quote(package)}', check=False)
        say(f'{package}: installing the APK with ' + (f'{res["driver"]}' if res['driver'] else 'no driver library (the device\'s driver)'))
        try: adb.install(out, package)
        except AdbError as e: raise BuildError(str(e)) from e
        env = f'{datapack.device_files(adb, package)}/env.txt'
        new = Path(t) / 'env.txt'; new.write_text(env_with_driver(adb.read_text(env), res['driver']), encoding='utf-8', newline='\n')
        adb.run('push', new, env)
    say(f'{package}: ' + (f'{ROW}={res["driver"]}' if res['driver'] else f'no {ROW} row: the device\'s driver') + ' (dropped: ' + (', '.join(res['dropped']) or 'none') + ')')
    return dict(res, package=package)


# ---- the drivers folder ----

def slot_name_ok(name):   # a name the app looks for in drivers/ (gfx_vk_core.c drv_slot: no / or .., ending in .so, 4 to 100 characters)
    return isinstance(name, str) and '/' not in name and '..' not in name and 4 <= len(name) <= 100 and name.endswith('.so')


def check_arm64(data, what):   # an ELF shared object for arm64 (64-bit, little-endian, e_machine 183 EM_AARCH64), from its first bytes
    if len(data) < 20 or data[:4] != b'\x7fELF': raise BuildError(f'{what}: not an ELF library')
    if data[4] != 2 or data[5] != 1 or int.from_bytes(data[18:20], 'little') != 183: raise BuildError(f'{what}: not an arm64 (aarch64) library')


def library_from(src):
    """(the driver library's bytes, its name) from a .so, or from an AdrenoTools driver package (.zip): the library its meta.json's libraryName names, else the zip's only
    libvulkan*.so / vulkan*.so"""
    src = Path(src)
    if not src.is_file(): raise BuildError(f'{src}: no such file')
    if src.suffix.lower() == '.so': return src.read_bytes(), src.name
    if src.suffix.lower() != '.zip': raise BuildError(f'{src}: neither a driver library (.so) nor a driver package (.zip)')
    try:
        with zipfile.ZipFile(src) as z:
            files = [i for i in z.infolist() if not i.is_dir()]
            meta = next((i for i in files if posixpath.basename(i.filename) == 'meta.json'), None)
            want = None
            if meta:
                try: want = json.loads(z.read(meta).decode('utf-8-sig')).get('libraryName')
                except (ValueError, AttributeError): raise BuildError(f'{src}: its meta.json is not a JSON object') from None
                if want is not None and not isinstance(want, str): raise BuildError(f'{src}: meta.json\'s libraryName is not a name')
            if want:
                hits = [i for i in files if i.filename == want] or [i for i in files if posixpath.basename(i.filename) == posixpath.basename(want)]
                if len(hits) != 1: raise BuildError(f'{src}: meta.json names {want}, which the package ' + ('does not hold' if not hits else 'holds more than once'))
            else:
                hits = [i for i in files if re.fullmatch(r'(lib)?vulkan.*\.so', posixpath.basename(i.filename))]
                if len(hits) != 1: raise BuildError(f'{src}: ' + ('no libvulkan*.so or vulkan*.so in it' if not hits else
                                                    'more than one driver library in it (' + ', '.join(i.filename for i in hits) + ') and no meta.json libraryName to choose'))
            return z.read(hits[0]), posixpath.basename(hits[0].filename)
    except zipfile.BadZipFile: raise BuildError(f'{src}: not a zip file') from None


def env_select(text, value):
    """env.txt with every DM_VK_DRIVER row (as the app reads keys: datapack.env_key) changed in place to DM_VK_DRIVER=value, each keeping its line ending; appended at the end
    when there is none. Every other line, comment and blank line stays as it is, in its order"""
    out, done = [], False
    for ln in text.splitlines(keepends=True):
        if datapack.env_key(ln) == ROW:
            body = ln.rstrip('\r\n'); out.append(f'{ROW}={value}' + ln[len(body):]); done = True
        else: out.append(ln)
    if not done:
        if out and not out[-1].endswith(('\n', '\r')): out.append('\n')
        out.append(f'{ROW}={value}\n')
    return ''.join(out)


def env_value(text):   # DM_VK_DRIVER as the app sees it (the last row wins), None without a row
    v = None
    for ln in text.splitlines():
        if datapack.env_key(ln) == ROW: v = ln.rstrip(' \r\n').split('=', 1)[1]
    return v


def drivers_folder(adb, package): return f'{datapack.device_files(adb, package)}/{DRIVERS}'   # (the data folder: internal storage, or the SD card the player chose)


def slots(adb, package):   # {name: size} of the drivers folder's files the app can load (its own folder level; a push's .part files left out)
    return {n: s for n, s in adb.remote_sizes(drivers_folder(adb, package)).items() if '/' not in n and not n.startswith('.') and slot_name_ok(n)}


def apk_driver(adb, package):
    """The driver library the installed APK carries, from its extracted library folder (as apkdriver.py tells it: any library not nx2android's own, but a validation layer);
    when that folder cannot be listed, the DM_VK_DRIVER row of the pack's env.txt as the app downloaded it (env.pack.txt, a port with --data-url). → (name, where it was read);
    '' as the name: the APK has none"""
    libdir = posixpath.dirname(installed_apk(adb, package)) + '/lib/arm64'
    lines = adb.shell(f'ls {shlex.quote(libdir)} 2>/dev/null; echo "rc=$?"', check=False).splitlines()
    if lines and lines[-1].strip() == 'rc=0':
        libs = [n.strip() for n in lines[:-1] if n.strip().endswith('.so') and n.strip() not in OWN and not n.strip().startswith('libVkLayer')]
        if len(libs) > 1: raise BuildError(f'{package}: the APK carries more than one library that could be its driver ({", ".join(libs)})')
        return (libs[0] if libs else ''), 'the APK'
    v = env_value(adb.read_text(f'{datapack.device_files(adb, package)}/env.pack.txt'))
    if v is not None: return v, 'the pack\'s env.txt as downloaded (env.pack.txt)'
    raise BuildError(f'{package}: the APK\'s driver is not known (its library folder {libdir} cannot be listed and there is no env.pack.txt): nothing to go back to; '
                     f'porter driver <library> puts a driver in the APK and names it')


def push_slot(adb, package, src, name=None):
    """A driver library (.so, or an AdrenoTools .zip) into the device's drivers folder as name (default: its own), without rebuilding the APK. → {'package', 'name', 'size'}"""
    data, own = library_from(src); name = name or own
    if not slot_name_ok(name): raise BuildError(f'{name}: not a name the drivers folder takes (no / or .., ending in .so, at most 100 characters)')
    check_arm64(data, f'{src}' + (f' ({own})' if own != Path(src).name else ''))
    try: apk_name = apk_driver(adb, package)[0]
    except BuildError: apk_name = ''
    if apk_name and name == apk_name: raise BuildError(f'{name} is the APK\'s driver library\'s name, so --select apk would load this one: give it another with --name')
    folder = drivers_folder(adb, package); remote, part = f'{folder}/{name}', f'{folder}/.{name}.part'
    with tempfile.TemporaryDirectory(prefix='nx2a_slot_') as t:
        local = Path(t) / name; local.write_bytes(data)
        adb.shell(f'mkdir -p {shlex.quote(folder)}')
        say(f'{package}: pushing {name} ({len(data) / 2**20:.1f} MB) to {folder}')
        adb.run('push', local, part, timeout=600)   # under another name first: an app starting meanwhile does not copy half a file
    got = adb.shell(f'stat -c %s {shlex.quote(part)} 2>/dev/null', check=False).strip()
    if got != str(len(data)): adb.shell(f'rm -f {shlex.quote(part)}', check=False); raise BuildError(f'{name}: the device has {got or "no"} bytes of it, not {len(data)}')
    adb.shell(f'mv -f {shlex.quote(part)} {shlex.quote(remote)}')
    say(f'{package}: {name} is in the drivers folder (porter driver --select {name} uses it from the next start)')
    return {'package': package, 'name': name, 'size': len(data)}


def select(adb, package, choice):
    """env.txt's DM_VK_DRIVER row: a file of the drivers folder (its name), 'apk' (the APK's own driver) or 'system' (DM_VK_DRIVER=, the device's driver). The app reads it
    at its next start. → {'package', 'selected', 'value', 'from'}"""
    env = f'{datapack.device_files(adb, package)}/{datapack.ENV}'
    text = adb.read_text(env)
    if not text.strip(): raise BuildError(f'{package}: no env.txt on the device ({env}): install the port first')
    have = slots(adb, package)
    if choice == 'system': value, kind, src = '', 'system', 'the device\'s driver'
    elif choice == 'apk':
        value, src = apk_driver(adb, package); kind = 'apk'
        if not value: raise BuildError(f'{package}: the APK carries no driver library: --select system for the device\'s driver')
        if value in have: raise BuildError(f'{package}: the drivers folder has a file named as the APK\'s driver ({value}), which the app would load instead: remove it first')
    else:
        if not slot_name_ok(choice): raise BuildError(f'{choice}: not a name the drivers folder takes (no / or .., ending in .so, at most 100 characters), nor apk or system')
        if choice not in have: raise BuildError(f'{choice} is not in the drivers folder ({drivers_folder(adb, package)}): ' + (', '.join(sorted(have)) or 'it holds none') + '; porter driver --push puts one there')
        value, kind, src = choice, 'drivers', 'the drivers folder'
    with tempfile.TemporaryDirectory(prefix='nx2a_env_') as t:
        new = Path(t) / datapack.ENV; new.write_text(env_select(text, value), encoding='utf-8', newline='')
        adb.run('push', new, env)
    say(f'{package}: {ROW}={value} ({src}), from the next start; {NOTE}')
    return {'package': package, 'selected': kind, 'value': value, 'from': src}


def listing(adb, package):
    """The drivers folder's files and what env.txt selects: → {'package', 'folder', 'drivers': [{'name', 'size'}], 'selected': 'drivers' | 'apk' | 'system', 'value'}"""
    have = slots(adb, package); v = env_value(adb.read_text(f'{datapack.device_files(adb, package)}/{datapack.ENV}')) or ''
    kind = 'system' if not v else 'drivers' if v in have else 'apk'   # as vk_init chooses: no value, the system's; a file of the folder by that name, it; else the APK's
    return {'package': package, 'folder': drivers_folder(adb, package), 'drivers': [{'name': n, 'size': s} for n, s in sorted(have.items())], 'selected': kind, 'value': v}
