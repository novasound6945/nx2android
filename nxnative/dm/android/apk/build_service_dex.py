#!/usr/bin/env python3
# build_service_dex.py: the background download's service (java_service/org/nx2a/DownloadService.java) compiled into service.dex beside this file, the dex mkapk.py puts in
# every APK (classes.dex, or classes2.dex beside a --dex of the APK's own) and porter apk-update gives a released APK. It is committed, so a port needs no JDK or Android
# SDK; service.dex.sha256 records the SHA-256 of the source it was built from (tests/test_bgdl_apk.py checks it is the source's now: run this after changing the source).
# Needs javac and an Android SDK with a platform (android.jar, API 29 or later) and build-tools (d8): ANDROID_HOME / ANDROID_SDK_ROOT, else ~/Library/Android/sdk,
# ~/Android/Sdk or %LOCALAPPDATA%/Android/Sdk.
# Usage: build_service_dex.py [--out DIR]   (DIR: where service.dex and service.dex.sha256 go; this folder by default)
import argparse, hashlib, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / 'java_service/org/nx2a/DownloadService.java'
DEX, RECORD = HERE / 'service.dex', HERE / 'service.dex.sha256'
MIN_API = 29   # mkapk.py MIN_SDK
MARK = b'Lorg/nx2a/DownloadService;'   # the class's descriptor in a dex: how porter apk-update finds the service's dex in an APK


def source_hash(): return hashlib.sha256(SOURCE.read_bytes().replace(b'\r\n', b'\n')).hexdigest()   # (a Windows checkout's CRLF: the same source)


def sdk_roots():
    env = [os.environ.get(k) for k in ('ANDROID_HOME', 'ANDROID_SDK_ROOT')]
    home = Path.home()
    return [Path(p) for p in env if p] + [home / 'Library/Android/sdk', home / 'Android/Sdk'] + ([Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'] if os.environ.get('LOCALAPPDATA') else [])


def tools():
    """(javac, android.jar, d8): the newest platform and build-tools found; SystemExit naming what is missing"""
    def version(p): return tuple(int(x) for x in re.findall(r'\d+', p.parent.name))
    roots = [r for r in sdk_roots() if r.is_dir()]
    jars = [p for r in roots for p in (r / 'platforms').glob('android-*/android.jar') if (version(p) or (0,))[0] >= MIN_API]
    d8s = [p for r in roots for p in (r / 'build-tools').glob('*/d8' + ('.bat' if os.name == 'nt' else ''))]
    javac = shutil.which('javac')
    if not javac or not jars or not d8s:
        raise SystemExit(f'build_service_dex.py needs javac and an Android SDK platform (API {MIN_API}+) and build-tools: found javac {javac or "none"}, '
                         f'{len(jars)} platform(s), {len(d8s)} d8 in {", ".join(map(str, roots)) or "no SDK folder"}')
    return javac, max(jars, key=version), max(d8s, key=version)


def build(out_dir=HERE):
    javac, jar, d8 = tools()
    out_dir = Path(out_dir); out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='nx2a_service_dex_') as t:
        classes, dex = Path(t) / 'classes', Path(t) / 'dex'; classes.mkdir(); dex.mkdir()
        subprocess.run([javac, '-encoding', 'UTF-8', '-source', '8', '-target', '8', '-Xlint:-options', '-classpath', str(jar), '-d', str(classes), str(SOURCE)], check=True)
        subprocess.run([str(d8), '--release', '--min-api', str(MIN_API), '--lib', str(jar), '--output', str(dex), *map(str, sorted(classes.rglob('*.class')))], check=True)
        data = (dex / 'classes.dex').read_bytes()
    if MARK not in data: raise SystemExit(f'{dex}: no {MARK.decode()} in the dex d8 wrote')
    (out_dir / DEX.name).write_bytes(data)
    (out_dir / RECORD.name).write_text(f'{source_hash()}  {SOURCE.relative_to(HERE.parent.parent.parent.parent).as_posix()}\n', encoding='utf-8', newline='\n')
    return out_dir / DEX.name, jar, d8


if __name__ == '__main__':
    a = argparse.ArgumentParser(); a.add_argument('--out', default=str(HERE)); args = a.parse_args()
    p, jar, d8 = build(args.out)
    print(f'{p}: {p.stat().st_size} bytes ({jar.parent.name}, {d8.parent.name}/d8), from {SOURCE.name} {source_hash()[:16]}…')
