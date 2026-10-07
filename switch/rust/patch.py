"""Teaches the Switch's copy of the libc crate about a 64-bit Horizon (the Switch): Rust
knows Horizon only as the 32-bit 3DS, whose C library lays several things out differently
and numbers several constants differently.

What is changed here is what switch/rust/layout-check found to disagree with the Switch's
own C headers; that check is the test of this file. The copy patched is a fresh one each
time (see setup.sh), and a piece of text that is no longer where it is expected (a newer
libc crate) stops the script.

    patch.py <the libc crate's folder>
"""
import re
import sys

libc = sys.argv[1]
NX = 'all(target_os = "horizon", target_arch = "aarch64")'


def replace(text, old, new, path):
    if old not in text:
        sys.exit(f'patch.py: {path} no longer has:\n{old}')
    return text.replace(old, new, 1)


# Constants whose value on the Switch is not the one the libc crate has for Horizon (or
# that it does not have for Horizon at all).
CONSTANTS = {
    'AF_INET6': 28, 'PF_INET6': 28, 'SO_ERROR': 0x1007, 'SOCK_CLOEXEC': 0x10000000, 'O_CLOEXEC': 0x40000,
    'MSG_DONTROUTE': 4, 'MSG_WAITALL': 0x40, 'MSG_DONTWAIT': 0x80, 'MSG_NOSIGNAL': 0x20000,
    'POLLOUT': 4, 'FIONBIO': 0x8004667E, 'CLOCK_BOOTTIME': 7,
    'TCP_NODELAY': 1, 'TCP_MAXSEG': 2, 'IP_TOS': 3, 'IP_TTL': 4, 'IP_ADD_MEMBERSHIP': 12, 'IP_DROP_MEMBERSHIP': 13,
    'AI_NUMERICSERV': 8, 'AI_ADDRCONFIG': 0x400, 'NI_NUMERICSERV': 8, 'NI_DGRAM': 16,
    'EAI_FAMILY': 5, 'EAI_MEMORY': 6, 'EAI_NONAME': 8, 'EAI_SOCKTYPE': 10, 'TRY_AGAIN': 2, 'NO_DATA': 4, 'NO_ADDRESS': 4,
}
definition = {name: re.compile(r'^([ \t]*)pub const %s: ([\w:]+) = [^;]+;\n' % name, re.M) for name in CONSTANTS}
types = {}

# --- newlib/mod.rs and unix/mod.rs: the types, and the constants stepped aside -----------
path = f'{libc}/src/unix/newlib/mod.rs'
text = open(path).read()
text = replace(text, '''pub type suseconds_t = i32;
''', '', path)
text = replace(text, '''pub type blkcnt_t = i32;
pub type blksize_t = i32;
''', '''cfg_if! {
    if #[cfg(%s)] {
        pub type blkcnt_t = i64;
        pub type blksize_t = i64;
        pub type suseconds_t = i64;
    } else {
        pub type blkcnt_t = i32;
        pub type blksize_t = i32;
        pub type suseconds_t = i32;
    }
}
''' % NX, path)
text = replace(text, '''    } else if #[cfg(any(target_arch = "arm", target_arch = "powerpc"))] {
        pub type dev_t = u32;
        pub type ino_t = u32;
        pub type off_t = i64;
''', '''    } else if #[cfg(%s)] {
        pub type dev_t = i16;
        pub type ino_t = u16;
        pub type off_t = i64;
    } else if #[cfg(any(target_arch = "arm", target_arch = "powerpc"))] {
        pub type dev_t = u32;
        pub type ino_t = u32;
        pub type off_t = i64;
''' % NX, path)
text = replace(text, '''    if #[cfg(target_os = "horizon")] {
        pub type sa_family_t = u16;
''', '''    if #[cfg(%s)] {
        pub type sa_family_t = u8;
    } else if #[cfg(target_os = "horizon")] {
        pub type sa_family_t = u16;
''' % NX, path)
# The thread library's structures: their sizes, and how one at rest is filled.
text = replace(text, '''cfg_if! {
    if #[cfg(target_os = "espidf")] {
        const __PTHREAD_INITIALIZER_BYTE: u8 = 0xff;
''', '''cfg_if! {
    if #[cfg(%s)] {
        const __PTHREAD_INITIALIZER_BYTE: u8 = 0;
        pub const __SIZEOF_PTHREAD_ATTR_T: usize = 24;
        pub const __SIZEOF_PTHREAD_MUTEX_T: usize = 12;
        pub const __SIZEOF_PTHREAD_MUTEXATTR_T: usize = 4;
        pub const __SIZEOF_PTHREAD_COND_T: usize = 16;
        pub const __SIZEOF_PTHREAD_CONDATTR_T: usize = 8;
        pub const __SIZEOF_PTHREAD_RWLOCK_T: usize = 16;
        pub const __SIZEOF_PTHREAD_RWLOCKATTR_T: usize = 8;
        pub const __SIZEOF_PTHREAD_BARRIER_T: usize = 32;
    } else if #[cfg(target_os = "espidf")] {
        const __PTHREAD_INITIALIZER_BYTE: u8 = 0xff;
''' % NX, path)
# A condition variable at rest names the clock its waits are timed by (the first thing
# in it): CLOCK_REALTIME, which is 1.
text = replace(text, '''pub const PTHREAD_COND_INITIALIZER: pthread_cond_t = pthread_cond_t {
    size: [__PTHREAD_INITIALIZER_BYTE; __SIZEOF_PTHREAD_COND_T],
};
''', '''#[cfg(not(%s))]
pub const PTHREAD_COND_INITIALIZER: pthread_cond_t = pthread_cond_t {
    size: [__PTHREAD_INITIALIZER_BYTE; __SIZEOF_PTHREAD_COND_T],
};
#[cfg(%s)]
pub const PTHREAD_COND_INITIALIZER: pthread_cond_t = pthread_cond_t {
    size: {
        let mut bytes = [0u8; __SIZEOF_PTHREAD_COND_T];
        bytes[0] = 1;
        bytes
    },
};
''' % (NX, NX), path)
text = replace(text, '''    } else if #[cfg(target_os = "horizon")] {
        mod horizon;
        pub use self::horizon::*;
''', '''    } else if #[cfg(%s)] {
        mod horizon_nx;
        pub use self::horizon_nx::*;
    } else if #[cfg(target_os = "horizon")] {
        mod horizon;
        pub use self::horizon::*;
''' % NX, path)


def step_aside(text):
    """Leaves each of CONSTANTS' definitions for every system but the Switch, whose own
    are in horizon_nx.rs."""
    for name, pattern in definition.items():
        for match in pattern.finditer(text):
            types.setdefault(name, match.group(2))
        text = pattern.sub(lambda m: f'{m.group(1)}#[cfg(not({NX}))]\n{m.group(0)}', text)
    return text


open(path, 'w').write(step_aside(text))
path = f'{libc}/src/unix/mod.rs'
text = step_aside(open(path).read())
open(path, 'w').write(text)

# --- newlib/horizon_nx.rs: the 3DS's module, with the Switch's structures and constants --
path = f'{libc}/src/unix/newlib/horizon/mod.rs'
text = open(path).read()
# The network structures are the BSD ones: each begins with its own length.
text = replace(text, '''        pub h_addrtype: u16,
        pub h_length: u16,
''', '''        pub h_addrtype: c_int,
        pub h_length: c_int,
''', path)
text = replace(text, '''        pub fd: c_int,
        pub events: c_int,
        pub revents: c_int,
''', '''        pub fd: c_int,
        pub events: c_short,
        pub revents: c_short,
''', path)
text = replace(text, '''        pub sa_family: crate::sa_family_t,
        pub sa_data: [c_char; 26usize],
''', '''        pub sa_len: u8,
        pub sa_family: crate::sa_family_t,
        pub sa_data: [c_char; 14usize],
''', path)
text = replace(text, '''        pub ss_family: crate::sa_family_t,
        __ss_padding: Padding<[c_char; 26usize]>,
''', '''        pub ss_len: u8,
        pub ss_family: crate::sa_family_t,
        __ss_pad1: Padding<[c_char; 6usize]>,
        __ss_align: Padding<i64>,
        __ss_pad2: Padding<[c_char; 112usize]>,
''', path)
text = replace(text, '''        pub sin_family: crate::sa_family_t,
''', '''        pub sin_len: u8,
        pub sin_family: crate::sa_family_t,
''', path)
# (All but the IPv6 address, which libnx declares without one.)
for name, pattern in definition.items():
    for match in pattern.finditer(text):
        types.setdefault(name, match.group(2))
    text = pattern.sub('', text)
text += '\n// The constants the Switch numbers differently from the 3DS (see switch/rust/patch.py).\n'
for name, value in CONSTANTS.items():
    text += f'pub const {name}: {types.get(name, "c_int")} = {value:#x};\n'
open(f'{libc}/src/unix/newlib/horizon_nx.rs', 'w').write(text)
print('patched', libc)
