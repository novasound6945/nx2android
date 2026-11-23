"""Android SDK build-tools for the tests that need aapt2 / apksigner / dexdump (not a test module): the highest version folder found, or an empty path."""
import os
import re
from pathlib import Path


def _version_key(name):
    nums = tuple(int(n) for n in re.findall(r'\d+', name.split('-')[0]))
    return nums, '-' not in name   # a release sorts above its own release candidate


def build_tools():
    """NX2A_BUILD_TOOLS when set, else the highest build-tools/* of ANDROID_HOME / ANDROID_SDK_ROOT (default ~/Library/Android/sdk) that holds aapt2;
    a path that is no folder when there is none (the callers skip)"""
    env = os.environ.get('NX2A_BUILD_TOOLS')
    if env: return Path(env)
    sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or Path.home() / 'Library/Android/sdk')
    found = sorted((d for d in (sdk / 'build-tools').glob('*') if (d / 'aapt2').is_file()), key=lambda d: _version_key(d.name))
    return found[-1] if found else sdk / 'build-tools' / 'none'
