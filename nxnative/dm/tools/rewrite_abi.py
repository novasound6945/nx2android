# The rewriter's output contract number (REWRITE_ABI, defined in dm/host/rewrite_abi.h: what it covers and when it is raised is written there), read for the Python tools,
# and the judgment of a data pack rewritten before the number was written (no # REWRITE_ABI in its fixups tables, no rewrite_abi in its nx2a-data.json).
# Such a pack's mods/build_src.sha256 holds the SHA-256 of the rewriter's sources that made it (patch_nso.py, nso_code.py, nso_asm.py): each is looked up in this checkout's git
# history, and every commit after it that changed the code of that file (comments and blank lines do not count: the tokens are compared) must be in COMMITS as format-neutral.
# Commits that already carry rewrite_abi.h are covered by the number itself (a format change raises it). Anything not found or not listed is "unknown", which callers treat as
# a different format: a port again from the game's files.
import hashlib, io, re, subprocess, tokenize
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
HEADER = TOOLS.parent / 'host/rewrite_abi.h'
CORE = ('patch_nso.py', 'nso_code.py', 'nso_asm.py')   # the rewriter itself (the patch files are judged by their tables: porter/apkupdate.py)
LEGACY = 1   # the contract of every Android output written before the number was (the rewriter from 9013fc91 on, COMMITS below)


def current(header=HEADER):
    """DM_REWRITE_ABI of rewrite_abi.h"""
    m = re.search(r'^#define DM_REWRITE_ABI (\d+)\s*$', Path(header).read_text(encoding='utf-8'), re.M)
    if not m: raise ValueError(f'{header}: no #define DM_REWRITE_ABI <number>')
    return int(m.group(1))


def from_fixups(text):
    """The number a <module>.fixups.tsv was written with, None for a table from before it"""
    m = re.search(r'^# REWRITE_ABI\t(\d+)\s*$', text, re.M)
    return int(m.group(1)) if m else None


# Every commit up to the number's introduction that changed the code of patch_nso.py, nso_code.py or nso_asm.py (git log --full-history; merges against their first parent),
# with its effect on the contract: 'same' (the output may differ, a host runs it as before) or 'format'. Commits that changed only comments are found by their tokens and need no row.
COMMITS = {
    '9d1c05ac29b0784a3ca05698db1b3b07edf9b587': ('format', 'nxnative imported: Apple only (slots through tpidrro_el0), no TARGET in the fixups table'),
    '9013fc91fb2c10ce8fa775c532d137a44ab3fc76': ('format', 'the Android target: slots through tpidr_el0[2], # TARGET android (the start of contract 1)'),
    '25b91f8f0bc4d01b9565f37dd7f7dddb34cfbc4b': ('same', 'DM_PATCHES_EXTRA cuts (--fps60), listed in cuts.tsv as any cut'),
    'bdfaa027acb4df7b759359bf3848fc365844f6ec': ('same', 'a br x18 after a popped frame taken as a tail call: which sites are rewritten'),
    '3d98ec0fbfce224b2f51fecbbd5ceb107647762d': ('same', 'CAS windows with an inner plain branch copied: which sites are rewritten'),
    '1ab0b6e696d0432ac8a1e88d09853e016e378133': ('same', 'a 64-bit cmp bounding a jump table index: which sites are rewritten'),
    '99df4393d9b834a53329fd2d77b3148ad698d45f': ('same', 'the frame step state page placed after the key page (FPS_STATE_OFF, read from the table)'),
    '51c9e7603b516db77d6ff7a24436d3898af96f38': ('same', 'float frame step: # FSTEP_FLOAT, a key the host reads when it is there; the patch area 64 KB on where the state page needs it (PATCH_OFF in the table)'),
    'e33aa4f8dfebd6cab043b725e342a9c82e4a2583': ('same', 'merge of 99df4393 / 1ab0b6e6 into the local line'),
    'd6412449f945aa2cebb27cf07e23e44492014e6c': ('same', '# NO_FRAME_STEP, a key the host ignores (removed again in 06a650f1)'),
    'caaf20fa4b14953ae6e21bca836b83a40228e83b': ('same', 'progress lines on stdout'),
    '06a650f1928be84bc40812d90f4f5b1f17c2a004': ('same', '# NO_FRAME_STEP dropped again (restack)'),
    '41cef5393f267bc8e8a910951b0c253fe2e8fb36': ('same', 'a give-up branch in an exclusive window takes the fall-through stub: the stub a branch goes to'),
    'f9d8d26c3a211a298b0fbebcd3db9119e0fd3403': ('same', 'merge of caaf20fa (progress lines)'),
    'df0f938a61ea84355e1c1640033ce38c36efaaf6': ('same', 'the fork\'s copy of 41cef539'),
}


def code_tokens(data):
    """A Python source's tokens without its comments and blank lines: two versions with the same tokens run the same"""
    return [(t.type, t.string) for t in tokenize.tokenize(io.BytesIO(data).readline) if t.type not in (tokenize.COMMENT, tokenize.NL, tokenize.ENCODING)]


def sha256s(data):   # a file's SHA-256 as it is and as a Windows checkout with CRLF line ends would hash it
    return {hashlib.sha256(data).hexdigest(), hashlib.sha256(data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')).hexdigest()}


def _git(*a, cwd=TOOLS):
    r = subprocess.run(['git', *a], cwd=cwd, capture_output=True)
    return r.stdout if r.returncode == 0 else None


def blob(commit, rel, cwd=TOOLS):   # a file's bytes at a commit (rel from cwd), None where it did not exist
    return _git('show', f'{commit}:./{rel}', cwd=cwd)


def history(rel, cwd=TOOLS):   # [(commit, first parent or '')] of the commits that changed rel, newest first
    out = _git('log', '--full-history', '--format=%H %P', '--', rel, cwd=cwd)
    return [(ln.split() + [''])[:2] for ln in out.decode().splitlines() if ln.strip()] if out is not None else None


def find_version(rel, sha, cwd=TOOLS):
    """The commits whose rel hashes to sha (as it is, or with CRLF line ends): [commit], None without git history"""
    h = history(rel, cwd)
    if h is None: return None
    found, seen = [], {}
    for c, _ in h:
        b = blob(c, rel, cwd)
        if b is not None and sha in seen.setdefault(hashlib.sha256(b).digest(), sha256s(b)): found.append(c)
    return found


def legacy_verdict(record, cwd=TOOLS):
    """record: {file name: sha256} of a pack's mods/build_src.sha256 (the rewriter's files, CORE). → (verdict, [reasons]): verdict 'same' (contract LEGACY), 'different'
    or 'unknown'. A file equal to this checkout's is the same rewriter; another is looked up in git and the commits after it judged (COMMITS)"""
    reasons, verdict = [], 'same'
    def worse(v):
        nonlocal verdict
        if v == 'different' or verdict == 'same': verdict = v
    for name in CORE:
        h = record.get(name)
        if not h: worse('unknown'); reasons.append(f'{name}: not in mods/build_src.sha256'); continue
        now = (Path(cwd) / name).read_bytes()
        if h in sha256s(now): reasons.append(f'{name}: the same as this checkout\'s'); continue
        found = find_version(name, h, cwd)
        if found is None: worse('unknown'); reasons.append(f'{name}: no git history in this checkout to look its version up in'); continue
        if not found: worse('unknown'); reasons.append(f'{name}: version {h[:12]} is in no commit of this checkout\'s history (another branch, or uncommitted changes)'); continue
        made = found[-1]   # the first commit with that content
        if code_tokens(blob(found[0], name, cwd)) == code_tokens(now):
            reasons.append(f'{name}: version of {made[:8]}, differs from this checkout\'s in comments only'); continue
        later = _git('rev-list', '--full-history', 'HEAD', '--not', *found, '--', name, cwd=cwd)
        later = later.decode().split() if later is not None else []
        bad = []
        for c in later:
            if blob(c, '../host/rewrite_abi.h', cwd) is not None: continue   # the number covers it: a format change raises it
            parent = (_git('rev-parse', f'{c}^1', cwd=cwd) or b'').decode().strip()
            a, b = blob(parent, name, cwd) if parent else None, blob(c, name, cwd)
            if a is not None and b is not None and code_tokens(a) == code_tokens(b): continue   # comments only
            effect = COMMITS.get(c)
            if effect is None: bad.append(('unknown', f'{c[:8]} (not judged in rewrite_abi.py COMMITS)'))
            elif effect[0] != 'same': bad.append(('different', f'{c[:8]}: {effect[1]}'))
        for v, why in bad: worse(v)
        reasons.append(f'{name}: version of {made[:8]}; ' + ('later code changes: ' + '; '.join(w for _, w in bad) if bad else f'{len(later)} later commit(s), none changes the output\'s contract'))
    return verdict, reasons
