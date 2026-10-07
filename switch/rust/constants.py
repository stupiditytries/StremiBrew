"""Compares the constants Rust's libc crate has for Horizon (written for the 3DS) with
what the Switch's C headers say, by having the Switch's compiler print each one.

Run on Windows (Git Bash), where devkitA64 is installed:
    python constants.py <a copy of libc's src/unix/newlib/horizon/mod.rs> <also newlib/mod.rs> ...
Prints, as Python, {name: value on the Switch} for every constant whose value differs,
and the names the Switch's headers do not define at all.
"""
import re
import subprocess
import sys
import tempfile
import os

DKP = os.environ.get('DEVKITPRO_WIN', 'C:/devkitPro')
HEADERS = '''
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
'''

names = {}
for path in sys.argv[1:]:
    for match in re.finditer(r'^\s*pub const (\w+): [\w:]+ = ([^;]+);', open(path).read(), re.M):
        names.setdefault(match.group(1), match.group(2).strip())

source = HEADERS
for name in names:
    if name.endswith('_INITIALIZER'):
        continue  # structures, not numbers
    source += f'#ifdef {name}\nconst long long val_{name} = (long long)({name});\n#endif\n'
with tempfile.TemporaryDirectory() as folder:
    c_file, s_file = os.path.join(folder, 'constants.c'), os.path.join(folder, 'constants.s')
    open(c_file, 'w').write(source)
    environment = dict(os.environ, DEVKITPRO=DKP)
    subprocess.run([f'{DKP}/devkitA64/bin/aarch64-none-elf-gcc.exe', '-S', '-O0', '-D__SWITCH__', '-D_GNU_SOURCE',
                    '-w', f'-I{DKP}/libnx/include', c_file, '-o', s_file], check=True, env=environment)
    assembly = open(s_file).read()

console = {m.group(1): int(m.group(2))
           for m in re.finditer(r'^val_(\w+):\n\s+\.(?:xword|quad)\s+(-?\d+)', assembly, re.M)}


def rust_value(text):
    text = text.replace('_', '') if re.fullmatch(r'[0-9_xa-fA-F]+', text) else text
    try:
        return int(text, 0)
    except ValueError:
        return None


differ, missing, unreadable = {}, [], []
for name, text in names.items():
    if name not in console:
        missing.append(name)
        continue
    value = rust_value(text)
    if value is None:
        unreadable.append((name, text, console[name]))
    elif value != console[name] and (value & 0xffffffff) != (console[name] & 0xffffffff):
        differ[name] = console[name]
print('differ =', differ)
print('unreadable =', unreadable)
print('missing =', missing)
