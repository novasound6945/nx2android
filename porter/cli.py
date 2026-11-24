# The command line (python -m porter …). Every command the GUI uses is here, so a port runs the same headless. Exit codes: 0 done, 1 error, 130 interrupted.
import argparse, json, os, sys
from pathlib import Path


def _parser():
    ap = argparse.ArgumentParser(prog='porter', description='Port your own Switch game files to an Android app (docs/ARCHITECTURE.md).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('doctor', help='check the tools a port needs'); d.add_argument('--json', action='store_true'); d.add_argument('-k', '--keys')
    p = sub.add_parser('profiles', help='list the games the tool can port'); p.add_argument('--all', action='store_true', help='also the iOS profiles not ported to Android'); p.add_argument('--json', action='store_true')
    i = sub.add_parser('inspect', help='what a game folder holds and whether it can be ported'); i.add_argument('folder', help="the game's folder, or one dump file (a merged NSP / XCI carries the base game, update and DLC)"); i.add_argument('--json', action='store_true')
    i.add_argument('-k', '--keys', help='prod.keys (default: the configured one): reads the title ID of a dump with no [title ID] in its name')
    po = sub.add_parser('port', help='make the APK and the data pack (and install them)')
    po.add_argument('folder', help='the game\'s folder, or one dump file: its base game NSP / XCI / NSZ / XCZ, updates and DLC (a merged NSP / XCI carries them all)')
    po.add_argument('-k', '--keys', help='prod.keys (default: the configured one)')
    po.add_argument('-o', '--out', help='folder for the APK and the data pack (default: the game folder)')
    po.add_argument('-p', '--version', default='', help='version to pack, v<N> (default: the newest in the folder)')
    po.add_argument('-l', '--language', default='', help='game language, a code or a name (default: English)')
    po.add_argument('--app-name', default='', help='the app\'s name on the device (default: the game\'s name)')
    po.add_argument('--install', action='store_true', help='install on the connected device afterwards')
    po.add_argument('--device', default='', help='adb serial of the device to install on')
    po.add_argument('--replace-signer', action='store_true', help='uninstall an app of the same package signed with another key first (deletes its saves)')
    po.add_argument('--debug', action='store_true', help='a development build: debug information, debuggable, DM_CTL key presses')
    po.add_argument('--experimental', action='store_true', help='try a game not ported to Android yet (a profile guessed from a survey of its files; it may not start)')
    po.add_argument('--package', default='', help="the app's package instead of the profile's (a test install beside another build of the game)")
    po.add_argument('--native-keyboard', action='store_true', help='use the Android Unicode keyboard (requires JDK and Android SDK platform/build-tools)')
    po.add_argument('--external-display', action='store_true', help='build the external-screen gamepad activity (requires Android SDK platform and build-tools)')
    po.add_argument('--fps60', action='store_true', help='60 fps: the profile\'s own frame-rate cap, else the community\'s FPSLocker-Warehouse config of the game\'s build, fetched from GitHub')
    po.add_argument('--touch', action='store_true',
                    help='a game played by touch: the on-screen gamepad starts hidden, so every touch but the menu button\'s goes to the game (the pause menu shows it again; '
                         'the first launch\'s default: the app keeps the player\'s own settings)')
    po.add_argument('--no-menu-button', action='store_true',
                    help='no on-screen menu button (DM_MENU_BUTTON=0 in env.txt): the Back gesture or key, or a controller\'s Home/Menu key, opens the pause menu; '
                         'it needs a port again (or env.txt edited) to show the button again')
    po.add_argument('--mt-replay', action='store_true',
                    help='multithread replay: the game\'s Vulkan commands are recorded on a thread of their own (DM_VK_MT_REPLAY=1 in env.txt), for a game whose replay '
                         'thread holds its frame rate back; to turn it off, port again without it and install (or edit env.txt)')
    po.add_argument('--vulkan-driver', default=None, metavar='FILE',
                    help='an arm64 Vulkan driver library (.so, such as Mesa\'s Turnip for Adreno) to pack in the APK and use instead of the device\'s own driver '
                         '(DM_VK_DRIVER in env.txt); the app falls back to the device\'s driver when the library does not load. A library not named lib*.so '
                         '(vulkan.purple.so) is packed as lib<name>, the only names Android extracts. A profile with vulkan_driver turnip gets Turnip without it (porter config turnip); '
                         '"system" keeps the device\'s driver')
    po.add_argument('--romfs-overlay', default=None, metavar='FOLDER',
                    help='a folder of files the game reads before its own RomFS (a mod laid out as the RomFS is: data/sound/..., replacing files or adding them): packed as '
                         'overlay/ beside the RomFS, which stays as dumped (DM_ROMFS_OVERLAY in env.txt); never shipped with the tool')
    po.add_argument('--shared-font', default=None, metavar='FILE',
                    help='a TrueType font (.ttf, one font, not a collection) to pack as the system font the game asks for, instead of the device\'s Roboto: one with CJK '
                         'characters shows the Chinese and Korean text of a game that draws it with the system font (DM_SHARED_FONT in env.txt); never shipped with the tool')
    po.add_argument('--firmware', default=None, metavar='DIR',
                    help="a folder of your console's system content files (.nca: its firmware dump, NAND SYSTEM Contents/registered) for a game that reads system data "
                         "(android.tsv sysdata: Mario Kart 8 Deluxe's Mii heads come from MiiModel); never shipped with the tool (default: the configured one)")
    po.add_argument('--vercode-offset', type=int, default=None, metavar='N',
                    help='add N (0-65535) to the APK\'s versionCode, so a rebuild of the same game version installs over a build with a higher one and keeps its saves '
                         '(default: $NX2A_VERCODE_OFFSET, else the build number of the game: one more than the last build of it on this computer). '
                         'N above 0 is also shown after the version of the game: 1.0.4-3')
    po.add_argument('--alias-headroom', type=int, default=None, metavar='GB',
                    help='GB (0-48) of extra guest address space (the alias region) for a game with virtual address memory, for a game whose log shows '
                         '"AllocateAddressRegion refused (0x1803)" before it crashes; it uses no memory (default: 8 for a game version the profile was not '
                         'brought up on, none for one it was)')
    _romfs_at(po, 'with --install: ')
    po.add_argument('--data-url', default='', metavar='URL',
                    help='the folder on a web server (http:// or https://, GET with Range) where the data pack is put as it is: the APK downloads its data from there at launch, '
                         'only the files that changed, and the pack gets the files.json the app compares (porter publish)')
    po.add_argument('--data-cred', default=None, metavar='FILE', help='with --data-url: a file of one line, user:password, for the server\'s Basic authentication (masked in the APK, never printed)')
    po.add_argument('--data-root', default='', metavar='URL',
                    help='with --data-pointer: the server folder the game packs are under; --data-url must be under it, and the rest of --data-url is this pack\'s folder, '
                         'kept when the pointer names another root')
    po.add_argument('--data-pointer', default='', metavar='URL',
                    help='the https:// address of a pointer file (porter pointer) that says where the data server is now: the app reads it first, so the server can move '
                         'without porting again; without an answer it uses the last pointer it read, else --data-url. Needs --data-root and --data-pointer-key')
    po.add_argument('--data-pointer-key', default=None, metavar='FILE', help='with --data-pointer: the key file the pointer is encrypted with (porter pointer --new-key; never printed)')
    po.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    po.add_argument('--progress', default='auto', choices=('auto', 'bar', 'lines', 'json', 'off'), help='json: one JSON event per line, for the GUI')
    ins = sub.add_parser('install', help='install a port made earlier'); ins.add_argument('data', help='the data pack folder (<App>-data)'); ins.add_argument('--apk', help='the APK (default: <App>.apk beside the pack)')
    ins.add_argument('--device', default=''); ins.add_argument('--replace-signer', action='store_true'); ins.add_argument('--progress', default='auto', choices=('auto', 'bar', 'lines', 'json', 'off'))
    _romfs_at(ins)
    pb = sub.add_parser('publish', help='write the data pack\'s files.json (paths, sizes, SHA-256), which an APK ported with --data-url compares and downloads by')
    pb.add_argument('data', help='the data pack folder (<App>-data)'); pb.add_argument('--progress', default='auto', choices=('auto', 'bar', 'lines', 'json', 'off'))
    pt = sub.add_parser('pointer', help='the pointer file an APK ported with --data-pointer reads the data server from: a new key, a pointer line, or a check of one')
    ptm = pt.add_mutually_exclusive_group(required=True)
    ptm.add_argument('--new-key', default=None, metavar='FILE', help='write a new random key into FILE (not over an existing one; the key is never printed)')
    ptm.add_argument('--root', default=None, metavar='URL', help='the https:// server folder the game packs are under now: prints the pointer line to put in the pointer file')
    ptm.add_argument('--check', default=None, metavar='FILE', help="open a pointer line (a file, or '-' for stdin): prints its server folder and whether it has credentials")
    pt.add_argument('--cred', default=None, metavar='FILE', help='with --root: the server\'s user:password file (as --data-cred; never printed)')
    pt.add_argument('--key', default=None, metavar='FILE', help='with --root or --check: the key file')
    pt.add_argument('-o', '--out', default=None, metavar='FILE', help='with --root: write the line into FILE instead of printing it')
    dv = sub.add_parser('devices', help='the devices adb sees'); dv.add_argument('--json', action='store_true')
    t = sub.add_parser('tools', help='list, or download, the missing tools (Android NDK, .NET SDK, platform-tools) from Google and Microsoft')
    t.add_argument('--download', action='store_true', help='download and unpack them into the nx2android folder (you accept their licenses)')
    t.add_argument('--only', default='', help='these instead of the missing ones: ndk, dotnet, adb (comma-separated)'); t.add_argument('--json', action='store_true')
    t.add_argument('--progress', default='auto', choices=('auto', 'bar', 'lines', 'json', 'off'))
    c = sub.add_parser('config', help='show or change the saved settings'); c.add_argument('key', nargs='?'); c.add_argument('value', nargs='?')
    sv = sub.add_parser('saves', help="a game's save between the device and this PC: list, export, import (a console's save too)")
    svs = sv.add_subparsers(dest='saves_cmd', required=True)
    sa = svs.add_parser('apps', help='the ported games installed on the device')
    sl = svs.add_parser('list', help="the save folders in the app's files folder (saves/), newest first")
    se = svs.add_parser('export', help="the game's save to a folder here: a fresh export (the app is stopped, started to export, and ends), or --name one already there")
    se.add_argument('dest', nargs='?', default='.', help='the folder to put it in (default: here)'); se.add_argument('--name', default='', help='a save folder on the device instead of a fresh export')
    si = svs.add_parser('import', help="a save folder or .zip as the game's save (a console's, as its save tools copy it, or an export); the save it replaces is exported first on the device")
    si.add_argument('src', help='the save folder (its files at the top, as the game keeps them) or a .zip of it; <folder>.device beside it is the device save')
    si.add_argument('--name', default='', help="its folder name on the device (default: the folder's or the .zip's)")
    for x in (sa, sl, se, si):
        x.add_argument('--device', default='', help='adb serial (default: the configured device, else the only one)'); x.add_argument('--json', action='store_true')
        if x is not sa: x.add_argument('--package', default='', help='the app (default: the only ported game installed)')
    dr = sub.add_parser('driver', help='another Vulkan driver for a port on the device, without the game files: its APK is rebuilt with the library and signed again (the saves stay); '
                                       '--push / --select / --list: test drivers in the app\'s drivers folder, no APK rebuilt')
    drg = dr.add_mutually_exclusive_group()   # one of these (cmd_driver checks), or --select alone
    drg.add_argument('library', nargs='?', help='the driver library (.so, such as Mesa\'s Turnip); the app must have been ported with --vulkan-driver once')
    drg.add_argument('--system', action='store_true', help='no driver library: the device\'s own driver')
    drg.add_argument('--push', default=None, metavar='FILE',
                     help='a test tool: put a driver library (.so, or an AdrenoTools driver package .zip) in the app\'s drivers folder on the device, without rebuilding the APK '
                          '(the APK\'s driver stays the reference); the app loads it once selected (--select), from its next start')
    drg.add_argument('--list', action='store_true', help='the drivers folder\'s libraries and which driver env.txt selects (the drivers folder\'s, the APK\'s or the system\'s)')
    dr.add_argument('--select', nargs='?', const='', default=None, metavar='NAME|apk|system',
                    help='env.txt\'s DM_VK_DRIVER: a library of the drivers folder, apk (the APK\'s own driver) or system (the device\'s); with --push and no value, the one pushed. '
                         'The next porter install may put the pack\'s DM_VK_DRIVER back (the pack\'s env.txt rows win): select again after an install')
    dr.add_argument('--name', default='', help='with --push: the library\'s name in the drivers folder (default: its own; no / or .., ending in .so)')
    dr.add_argument('--package', default='', help='the app (default: the only ported game installed)')
    dr.add_argument('--device', default='', help='adb serial (default: the configured device, else the only one)'); dr.add_argument('--json', action='store_true')
    hs = sub.add_parser('host', help="a port on the device brought to this version's host, without the game files: its libdmhost.so is built again from the profile (the saves and the game's data stay)")
    hs.add_argument('--package', default='', help='the app (default: the only ported game installed)')
    hs.add_argument('--device', default='', help='adb serial (default: the configured device, else the only one)')
    hs.add_argument('--jobs', type=int, default=os.cpu_count() or 8); hs.add_argument('--json', action='store_true')
    au = sub.add_parser('apk-update', help="a released port's APK again with this version's host and a new build number, without the game files: the data pack on the server "
                                           "stays (its rewritten code must fit the host: --check says whether it does)")
    au.add_argument('apk', nargs='?', help='the released APK (a file); with --check it may be left out when --data is given')
    au.add_argument('--data', default='', metavar='URL|FOLDER',
                    help="the game's data pack: its folder on the web server (http(s)://…/, as the APK downloads it) or a local copy of it (nx2a-data.json, files.json, mods/); "
                         "default: the server address in the APK (nx2a/source.bin)")
    au.add_argument('--data-cred', default=None, metavar='FILE', help="a file of one line, user:password, for the server (default: the APK's own; never printed)")
    au.add_argument('--build', type=int, default=None, metavar='N',
                    help="the build number (0-65535), shown after the game's version (1.0.1-8) and added to the versionCode; default: one more than the APK's and this computer's last")
    au.add_argument('-o', '--out', default=None, metavar='FOLDER', help='where the new APK (and a changed nx2a-data.json with its files.json) go (default: ./apk-update)')
    au.add_argument('--profile-header', default=None, metavar='FILE',
                    help="the released host's game_profile.h (its port's build folder, android/host/gen/<title ID>/), for a pack whose profile git does not hold (a derived profile's port)")
    au.add_argument('--package', default=None, metavar='NAME',
                    help="the APK's new package (another app beside the old one, with its own files and saves; the data pack on the server stays as it is and the build "
                         "number goes on from the old package's): the label, the permissions and the services are kept as for any update")
    au.add_argument('--check', action='store_true', help='only judge whether the data pack fits this version\'s host (same / different / unknown, with the reasons): no build, no APK')
    au.add_argument('--jobs', type=int, default=os.cpu_count() or 8); au.add_argument('--json', action='store_true')
    pf = sub.add_parser('pack-add-font', help="a published data pack's changed files with the player's shared font added, as porter port --shared-font makes them (standard.ttf, "
                                              "env.txt's DM_SHARED_FONT row, nx2a-data.json, files.json), without porting the game again; nothing is uploaded")
    pf.add_argument('data', metavar='URL|FOLDER', help="the data pack: its folder on the web server (http(s)://…/) or a local copy of it (nx2a-data.json, files.json, env.txt)")
    pf.add_argument('--shared-font', required=True, metavar='FILE', help='the TrueType font, added as it is (standard.ttf)')
    pf.add_argument('--data-cred', default=None, metavar='FILE', help='a file of one line, user:password, for the server (never printed)')
    pf.add_argument('-o', '--out', default=None, metavar='FOLDER', help='where the changed files go, to upload into the pack\'s folder, files.json last (default: ./pack-add-font)')
    pf.add_argument('--json', action='store_true')
    return ap


def _romfs_at(p, when=''):   # install's --romfs-at / --romfs-push (porter/install.py)
    p.add_argument('--romfs-at', default=None, metavar='FOLDER',
                   help=when + 'read the game\'s RomFS from this folder on the device (e.g. /sdcard/Games/<game>/romfs, a dump another app also reads, or a folder on the SD card) '
                        'instead of a copy in the app\'s files folder: the folder must hold exactly the pack\'s RomFS (names and sizes, and the content of a sample), the app is given All files access, '
                        'and a copy an earlier install left in the files folder is deleted. Later installs keep the folder; "app" moves the RomFS back into the files folder; "sd" pushes it into <the SD card>/Android/data/<package>/files/RomFS, the app\'s own folder there (Android removes it when the app is uninstalled; the APK stays in internal storage; later installs push what changed into it and remove files that are not the pack\'s; a USB drive only when no SD card is mounted); "no-sd" moves data an earlier "sd" install put on the card back into the app files folder, else changes nothing')
    p.add_argument('--romfs-push', action='store_true', help='with --romfs-at: push the pack\'s RomFS into that folder first (an empty folder, or one of this game; nothing there is deleted)')


def _print_json(obj): print(json.dumps(obj, ensure_ascii=False, indent=2))


def cmd_doctor(a):
    from porter import toolchain
    rows = toolchain.checks(keys=a.keys)
    if a.json: _print_json([r.as_dict() for r in rows])
    else:
        for r in rows: print(f'{"ok " if r.ok else ("!! " if r.required else "-- ")} {r.name:15} {r.detail}' + (f'\n{"":19}→ {r.fix}' if r.fix and not r.ok else ''))
    return 0 if all(r.ok or not r.required for r in rows) else 1


def cmd_profiles(a):
    from porter import profiles
    rows = profiles.all_entries(android_only=not a.all)
    if a.json: _print_json([r.as_dict() for r in rows]); return 0
    for r in rows: print(f'{r.id}  {r.status:12} {r.name}' + (f'  ({r.tested})' if r.tested else ''))
    return 0


def cmd_inspect(a):
    import contextlib
    from porter import pipeline
    from porter.upstream import BuildError
    try:
        with contextlib.redirect_stdout(sys.stderr if a.json else sys.stdout):
            info = pipeline.inspect(a.folder, a.keys)   # keep JSON stdout one document for the GUI
    except BuildError as e:
        if a.json: _print_json({'error': str(e)})
        else: print(e, file=sys.stderr)
        return 2
    if a.json: _print_json(info); return 0 if info['supported'] else 2
    print(f'{info["title_id"]}  {info.get("name", "")}\n  base: {info["base"]}')
    for u in info['updates']: print(f'  update {u["version"]}: {u["file"]}')
    for d in info['dlc']: print(f'  DLC {d["title_id"]}: {d["file"]}')
    if info.get('pack'): print(f'  packs {info["pack"]} (versions in the folder: {", ".join(info["versions"])})')
    if info['android']: print(f'  Android: {info["android"]["status"]}' + (f', tested on {info["android"]["tested"]}' if info['android']['tested'] else ''))
    if info['reason']: print(f'  {info["reason"]}')
    if info.get('experimental'): print('  porter port --experimental tries it')
    return 0 if info['supported'] else 2


def _with_progress(mode, fn):
    from porter.upstream import BuildError, PROG, nsp
    PROG.mode = nsp.progress_mode(mode)
    environ = dict(os.environ)
    try:
        fn(); return 0
    except BuildError as e:
        PROG.close(ok=False); PROG.say(f'[RED] {e}'); return 1
    except KeyboardInterrupt:
        PROG.close(ok=False); PROG.say('[RED] interrupted'); return 130
    except Exception as e:
        PROG.close(ok=False)
        if PROG.mode == 'json': PROG.say(f'[RED] {type(e).__name__}: {e}')
        raise
    finally:
        PROG.close(ok=False)
        for k in os.environ.keys() - environ.keys(): del os.environ[k]
        os.environ.update(environ)


def cmd_port(a):
    from porter import pipeline
    offset = a.vercode_offset
    if offset is None and os.environ.get('NX2A_VERCODE_OFFSET', '').strip():
        env = os.environ['NX2A_VERCODE_OFFSET'].strip()
        try: offset = int(env)
        except ValueError: print(f'NX2A_VERCODE_OFFSET is not a number: {env}', file=sys.stderr); return 2
    if (a.romfs_at is not None or a.romfs_push) and not a.install: print('--romfs-at and --romfs-push are install options: add --install, or run porter install after the port', file=sys.stderr); return 2
    from porter import install
    from porter.upstream import BuildError
    try: install.romfs_target(a.romfs_at)   # a folder the app cannot read fails now, not after the shaders
    except BuildError as e: print(e, file=sys.stderr); return 2
    opts = pipeline.Options.from_args(a, vercode_offset=offset)   # every other field from the argument of its name
    return _with_progress(a.progress, lambda: pipeline.emit_result(pipeline.Port(opts).run()))


def cmd_install(a):
    from porter import config, datapack, install, toolchain
    from porter.upstream import BuildError, PROG
    def go():
        data = Path(a.data).resolve()
        apk = Path(a.apk) if a.apk else data.with_name(data.name.removesuffix('-data') + '.apk')
        PROG.plan(1)
        cfg = config.load()
        install.install(apk, data, toolchain.find_adb(cfg), a.device or cfg.get('device'), replace_signer=a.replace_signer, romfs_at=a.romfs_at, romfs_push=a.romfs_push)
    return _with_progress(a.progress, go)


def cmd_publish(a):
    from porter import publish
    from porter.upstream import PROG, say
    def go():
        PROG.plan(1); PROG.stage('publish', step='hashing the files')
        fj = publish.manifest(Path(a.data), progress=lambda d, t: PROG.set(done=d, total=t, unit='B'))
        PROG.end(f'{publish.FILES}: {len(fj["files"]):,} files, {fj["bytes"] / 2**30:.2f} GB, {fj["hashed"]:,} hashed, id {fj["id"][:16]}')
        say(f'upload {Path(a.data).resolve()} as it is (files.json last) to the folder the APK was ported with (--data-url)')
    return _with_progress(a.progress, go)


def cmd_pointer(a):   # porter pointer: the key and the line never reach the screen but as the line itself (which is encrypted)
    from porter import publish
    from porter.upstream import BuildError
    try:
        if a.new_key is not None:
            if a.cred or a.key or a.out: raise BuildError('--new-key takes no other option')
            p = publish.new_key(a.new_key); print(f'a new pointer key is in {p}: keep it, the APKs ported with --data-pointer-key {p} need it to read the pointer')
            return 0
        if not a.key: raise BuildError('--root and --check need --key FILE')
        key = publish.read_key(a.key, '--key')
        if a.root is not None:
            line = publish.pointer_seal(a.root, publish.read_cred(a.cred) if a.cred else '', key)
            if a.out:
                try: Path(a.out).write_text(line + '\n', encoding='ascii', newline='\n')
                except OSError as e: raise BuildError(f'cannot write {a.out} ({e.strerror})') from None
                print(f'the pointer line is in {a.out}: put it as the pointer file', file=sys.stderr)
            else: print(line)
            return 0
        if a.cred or a.out: raise BuildError('--check takes only --key')
        try: text = sys.stdin.read() if a.check == '-' else Path(a.check).read_text(encoding='ascii', errors='replace')
        except OSError as e: raise BuildError(f'cannot read {a.check} ({e.strerror})') from None
        root, cred = publish.pointer_open(text, key)
        print(f'{root} ({"with credentials" if cred else "no credentials"})')
        return 0
    except BuildError as e:
        print(e, file=sys.stderr); return 1


def cmd_devices(a):
    from porter import config, toolchain
    from porter.adb import Adb, AdbError
    exe = toolchain.find_adb()
    if not exe: print('adb not found', file=sys.stderr); return 1
    try: rows = Adb(exe).devices()
    except AdbError as e: print(e, file=sys.stderr); return 1
    if a.json: _print_json([{'serial': s, 'state': st, 'info': d} for s, st, d in rows]); return 0
    for s, st, d in rows: print(f'{s}\t{st}\t{d}')
    if not rows: print('no device')
    return 0


def cmd_tools(a):
    from porter import fetch
    from porter.upstream import BuildError
    names = [{'ndk': 'Android NDK', 'dotnet': '.NET SDK', 'adb': 'adb'}[n.strip()] for n in a.only.split(',') if n.strip()] or None
    if a.download:
        def go():
            items = fetch.plan(names)
            if not items: fetch.PROG.say('nothing to download: every tool is there'); return
            for it in items: fetch.PROG.say(f'{it.name} {it.version} ({it.size / 2**20:,.0f} MB) from {it.url}, license: {it.license}')
            fetch.download(items)
        return _with_progress(a.progress, go)
    try: items = fetch.plan(names)
    except (BuildError, OSError) as e: print(f'cannot read the download lists: {e}', file=sys.stderr); return 1
    if a.json: _print_json([it.as_dict() for it in items]); return 0
    for it in items: print(f'{it.name:12} {it.version:16} {it.size / 2**20:8,.0f} MB  {it.url}\n{"":12} license: {it.license}')
    print('porter tools --download fetches them' if items else 'nothing to download: every tool is there')
    return 0


def cmd_config(a):
    from porter import config
    if a.key is None:
        for k, v in config.load().items(): print(f'{k:9} {v or "-":40}  {config.KEYS[k]}')
        print(f'({config.path()})'); return 0
    if a.value is None: print(config.load().get(a.key, '')); return 0
    try: config.set_value(a.key, a.value)
    except KeyError as e: print(e.args[0], file=sys.stderr); return 1
    return 0


def cmd_saves(a):
    import contextlib
    from porter import config, saves, toolchain
    from porter.upstream import BuildError
    try:
        with contextlib.redirect_stdout(sys.stderr if a.json else sys.stdout):   # with --json, stdout is the one JSON document (the GUI parses it whole)
            cfg = config.load(); adb = saves.connect(toolchain.find_adb(cfg), a.device or cfg.get('device'))
            if a.saves_cmd == 'apps': out = saves.apps(adb)
            else:
                pkg = saves.pick_app(adb, a.package)
                if a.saves_cmd == 'list': out = saves.listing(adb, pkg)
                elif a.saves_cmd == 'export': out = {'package': pkg, 'folder': str(saves.export(adb, pkg, Path(a.dest), a.name))}
                else: out = dict(saves.import_(adb, pkg, Path(a.src), a.name), package=pkg)
    except BuildError as e:
        if a.json: _print_json({'error': str(e)})
        else: print(e, file=sys.stderr)
        return 1
    if a.json: _print_json(out)
    elif a.saves_cmd == 'apps': print('\n'.join(out) or 'no ported game is installed')
    elif a.saves_cmd == 'list':
        import time
        for r in out: print(f'{time.strftime("%Y-%m-%d %H:%M", time.localtime(r["time"]))}  {r["name"]:32} {r["files"]:5} files {r["bytes"] / 2**20:9.2f} MB' + ('  (+ device save)' if r['device'] else ''))
        if not out: print(f'no save folders on the device ({saves.saves_folder(pkg)})')
    return 0


def _driver_mode(a):   # porter driver's mode: 'swap' (a library, or --system: the APK rebuilt), 'push', 'select', 'list'; or the usage error
    if a.cmd != 'driver': return 'host', None
    if a.push is not None: mode = 'push'
    elif a.list: mode = 'list'
    elif a.library is not None or a.system: mode = 'swap'
    elif a.select is not None: mode = 'select'
    else: return None, 'give a driver library, --system, --push FILE, --select NAME|apk|system or --list'
    if mode == 'push' and a.select: return None, '--push takes --select without a value (the library it pushes)'
    if mode == 'select' and not a.select: return None, '--select needs NAME, apk or system (without one only after --push)'
    if mode in ('swap', 'list') and a.select is not None: return None, '--select goes alone, or with --push'
    if a.name and mode != 'push': return None, '--name goes with --push'
    return mode, None


def cmd_driver(a):   # porter driver and porter host: the installed APK rebuilt without the game files; porter driver --push / --select / --list: the drivers folder (no APK rebuilt)
    import contextlib
    from porter import config, driver, host, paths, saves, toolchain
    from porter.adb import AdbError
    from porter.upstream import BuildError
    mode, err = _driver_mode(a)
    if err: print(f'porter driver: {err}', file=sys.stderr); return 2
    try:
        with contextlib.redirect_stdout(sys.stderr if a.json else sys.stdout):   # with --json, stdout is the one JSON document
            cfg = config.load(); adb = saves.connect(toolchain.find_adb(cfg), a.device or cfg.get('device'))
            lock = adb.hold(paths.user_dir()) if mode != 'list' else None   # they reinstall the APK or write env.txt: not while porter install runs on the device
            try:
                pkg = saves.pick_app(adb, a.package)
                if mode == 'swap': out = driver.swap(adb, pkg, None if a.system else a.library)
                elif mode == 'host': out = host.update(adb, pkg, a.jobs)
                else:
                    try:
                        if mode == 'push':
                            out = driver.push_slot(adb, pkg, a.push, a.name or None)
                            if a.select is not None: out['select'] = driver.select(adb, pkg, out['name'])
                        elif mode == 'select': out = driver.select(adb, pkg, a.select)
                        else: out = driver.listing(adb, pkg)
                    except AdbError as e: raise BuildError(str(e)) from e
            finally:
                if lock: lock.close()
    except BuildError as e:
        if a.json: _print_json({'error': str(e)})
        else: print(e, file=sys.stderr)
        return 1
    if a.json: _print_json(out)
    elif mode == 'list':
        for d in out['drivers']: print(f'{d["name"]:40} {d["size"] / 2**20:8.1f} MB' + ('   (selected)' if out['selected'] == 'drivers' and d['name'] == out['value'] else ''))
        if not out['drivers']: print(f'no driver libraries in {out["folder"]} (porter driver --push puts one there)')
        print('selected: ' + {'drivers': f'{out["value"]}, from the drivers folder', 'apk': f'the APK\'s driver ({out["value"]})', 'system': 'the device\'s driver (no DM_VK_DRIVER value)'}[out['selected']])
        print(driver.NOTE)
    return 0


def cmd_apk_update(a):   # porter apk-update: the released APK with this checkout's host (porter/apkupdate.py); nothing is uploaded or installed
    import contextlib
    from porter import apkupdate, publish
    from porter.upstream import BuildError
    try:
        with contextlib.redirect_stdout(sys.stderr if a.json else sys.stdout):   # with --json, stdout is the one JSON document
            if not a.apk and not (a.check and a.data): raise BuildError('give the released APK (with --check, --data alone will do)')
            if a.apk and not Path(a.apk).is_file(): raise BuildError(f'{a.apk}: no such APK file')
            url, cred = apkupdate.source_of(a.apk) if a.apk else (None, '')
            where = a.data or url
            if not where: raise BuildError(f'{a.apk} has no data server address (ported without --data-url): give --data URL|FOLDER')
            if a.data_cred: cred = publish.read_cred(a.data_cred)
            elif a.data and url and not a.data.startswith(url): cred = ''   # the APK's credentials only for its own server
            pack = apkupdate.Pack(where, cred)
            try: old_header = Path(a.profile_header).read_bytes() if a.profile_header else None
            except OSError as e: raise BuildError(f'--profile-header: cannot read {a.profile_header} ({e.strerror})') from None
            if a.check:
                out = apkupdate.judge(pack, old_header); out.pop('manifest', None)
                if a.apk:   # the APK's own facts beside the judgment
                    out['apk'] = {k: v for k, v in apkupdate.axml.apk_info(a.apk).items() if k != 'activity'}
                    if apkupdate.data_package(a.apk) != out['package']: out.update(ok=False, verdict='different'); out.setdefault('errors', []).append(f'the APK is {out["apk"]["package"]}, the pack {out["package"]}')
                if not a.json:
                    for ln in apkupdate.report_lines(out): print(ln)
                    if out.get('errors'): print('  ' + '; '.join(out['errors']))
            else:
                out = apkupdate.update(a.apk, pack, Path(a.out or 'apk-update'), a.build, a.jobs, old_header, a.package)
    except BuildError as e:
        if a.json: _print_json({'error': str(e)})
        else: print(e, file=sys.stderr)
        return 1
    if a.json: _print_json(out)
    return 0 if out.get('ok', True) else 3


def cmd_pack_add_font(a):   # porter pack-add-font: the shared font added to a published pack (porter/packfont.py); nothing is uploaded
    import contextlib
    from porter import apkupdate, packfont, publish
    from porter.upstream import BuildError
    try:
        with contextlib.redirect_stdout(sys.stderr if a.json else sys.stdout):
            pack = apkupdate.Pack(a.data, publish.read_cred(a.data_cred) if a.data_cred else '')
            out = packfont.add_font(pack, a.shared_font, Path(a.out or 'pack-add-font'))
            if out['same'] and not a.json: print(f'{pack}: has this {packfont.FONT} and its {packfont.KEY} row already: nothing to upload')
    except BuildError as e:
        if a.json: _print_json({'error': str(e)})
        else: print(e, file=sys.stderr)
        return 1
    if a.json: _print_json(out)
    return 0


def main(argv):
    if hasattr(sys.stdout, 'reconfigure'): sys.stdout.reconfigure(encoding='utf-8', errors='replace')   # game names and progress bars on a Windows console
    a = _parser().parse_args(argv)
    return {'doctor': cmd_doctor, 'profiles': cmd_profiles, 'inspect': cmd_inspect, 'port': cmd_port, 'install': cmd_install, 'publish': cmd_publish, 'pointer': cmd_pointer, 'devices': cmd_devices, 'tools': cmd_tools, 'config': cmd_config, 'saves': cmd_saves, 'driver': cmd_driver, 'host': cmd_driver, 'apk-update': cmd_apk_update, 'pack-add-font': cmd_pack_add_font}[a.cmd](a)
