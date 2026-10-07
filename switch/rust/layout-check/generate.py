"""Writes checks.rs: for every constant the libc crate could have for Horizon, and for
the structures Rust's standard library passes to the C library, an assertion that the
libc crate agrees with the Switch's own C headers. Compiling this crate for the Switch
(check.sh) then names every disagreement.

Run on Windows (Git Bash), where devkitA64 and libnx are installed:
    python generate.py <copies of libc's newlib/horizon/mod.rs, newlib/mod.rs, unix/mod.rs>
"""
import os
import re
import subprocess
import sys
import tempfile

DKP = os.environ.get('DEVKITPRO_WIN', 'C:/devkitPro')
HEADERS = '''
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
'''
# Structures and plain types: (the name in Rust and C, "struct " or "" in C, fields).
STRUCTS = [
    ('stat', 'struct ', ['st_dev', 'st_ino', 'st_mode', 'st_nlink', 'st_uid', 'st_gid', 'st_rdev', 'st_size',
                         'st_atim', 'st_mtim', 'st_ctim', 'st_blksize', 'st_blocks']),
    ('dirent', 'struct ', ['d_ino', 'd_type', 'd_name']),
    ('timespec', 'struct ', ['tv_sec', 'tv_nsec']),
    ('timeval', 'struct ', ['tv_sec', 'tv_usec']),
    ('sockaddr', 'struct ', ['sa_family', 'sa_data']),
    ('sockaddr_in', 'struct ', ['sin_family', 'sin_port', 'sin_addr']),
    ('sockaddr_in6', 'struct ', ['sin6_family', 'sin6_port', 'sin6_flowinfo', 'sin6_addr', 'sin6_scope_id']),
    ('sockaddr_storage', 'struct ', ['ss_family']),
    ('addrinfo', 'struct ', ['ai_flags', 'ai_family', 'ai_socktype', 'ai_protocol', 'ai_addrlen', 'ai_canonname',
                             'ai_addr', 'ai_next']),
    ('pollfd', 'struct ', ['fd', 'events', 'revents']),
    ('linger', 'struct ', ['l_onoff', 'l_linger']),
    ('in_addr', 'struct ', ['s_addr']),
    ('in6_addr', 'struct ', []),
    ('ip_mreq', 'struct ', ['imr_multiaddr', 'imr_interface']),
    ('iovec', 'struct ', ['iov_base', 'iov_len']),
    ('tm', 'struct ', ['tm_sec', 'tm_year', 'tm_isdst']),
    ('sched_param', 'struct ', ['sched_priority']),
] + [(name, '', []) for name in [
    'time_t', 'suseconds_t', 'clockid_t', 'mode_t', 'nlink_t', 'uid_t', 'gid_t', 'dev_t', 'ino_t', 'off_t',
    'blksize_t', 'blkcnt_t', 'pid_t', 'ssize_t', 'size_t', 'socklen_t', 'sa_family_t', 'in_port_t', 'in_addr_t',
    'nfds_t', 'pthread_t', 'pthread_key_t', 'clock_t',
]]
# Opaque in Rust: only their size matters, and Rust's may be the larger.
OPAQUE = ['pthread_attr_t', 'pthread_mutex_t', 'pthread_mutexattr_t', 'pthread_cond_t', 'pthread_condattr_t',
          'pthread_rwlock_t', 'pthread_rwlockattr_t']

constants = []
for path in sys.argv[1:]:
    for match in re.finditer(r'^\s*pub const (\w+): [\w:]+ =', open(path).read(), re.M):
        name = match.group(1)
        if name not in constants and not name.endswith('_INITIALIZER') and not name.startswith('__'):
            constants.append(name)

source = HEADERS
for name in constants:
    source += f'#ifdef {name}\nconst long long val_{name} = (long long)({name});\n#endif\n'
for name, prefix, fields in STRUCTS:
    source += f'const long long size_{name} = sizeof({prefix}{name});\n'
    for field in fields:
        source += f'const long long off_{name}__{field} = offsetof({prefix}{name}, {field});\n'
for name in OPAQUE:
    source += f'const long long size_{name} = sizeof({name});\n'
with tempfile.TemporaryDirectory() as folder:
    c_file, s_file = os.path.join(folder, 'layout.c'), os.path.join(folder, 'layout.s')
    open(c_file, 'w').write(source)
    subprocess.run([f'{DKP}/devkitA64/bin/aarch64-none-elf-gcc.exe', '-S', '-O0', '-D__SWITCH__', '-D_GNU_SOURCE',
                    '-w', f'-I{DKP}/libnx/include', c_file, '-o', s_file], check=True,
                   env=dict(os.environ, DEVKITPRO=DKP))
    assembly = open(s_file).read()
found = {m.group(1): int(m.group(2))
         for m in re.finditer(r'^(\w+):\n\s+\.(?:xword|quad)\s+(-?\d+)', assembly, re.M)}
# A zero is written as so many bytes of nothing.
found.update({m.group(1): 0 for m in re.finditer(r'^(\w+):\n\s+\.zero\s+8', assembly, re.M)})

lines = ['// Written by generate.py from the Switch\'s C headers; not edited by hand.',
         '#![allow(unused, clippy::all)]', 'use core::mem::{offset_of, size_of};', '']
for name in constants:
    if f'val_{name}' in found:
        value = found[f'val_{name}']
        # Compared as 32 bits: C's unsigned constants are signed ones in Rust.
        lines.append(f'const _: () = assert!(libc::{name} as i64 as u32 == {value & 0xffffffff}u32);')
for name, _, fields in STRUCTS:
    lines.append(f'const _: () = assert!(size_of::<libc::{name}>() == {found[f"size_{name}"]});')
    for field in fields:
        lines.append(f'const _: () = assert!(offset_of!(libc::{name}, {field}) == {found[f"off_{name}__{field}"]});')
for name in OPAQUE:
    lines.append(f'const _: () = assert!(size_of::<libc::{name}>() >= {found[f"size_{name}"]});')
here = os.path.dirname(os.path.abspath(__file__))
open(os.path.join(here, 'src', 'checks.rs'), 'w', newline='\n').write('\n'.join(lines) + '\n')
print(len(lines) - 4, 'checks written')
