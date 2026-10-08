"""Makes the Switch toolchain's standard library build against the same version of the
libc crate as everything else (the one patch.py corrects for the Switch): the lock file
that comes with the standard library's source names an older one, which knows nothing of
the corrections.

    pin-libc.py <the standard library's Cargo.lock> <a Cargo.lock naming the wanted version> <version>
Prints "changed" when it changed the file.
"""
import re
import sys

std_lock, our_lock, version = sys.argv[1], sys.argv[2], sys.argv[3]
block = re.compile(r'\[\[package\]\]\nname = "libc"\nversion = "([^"]+)"\nsource = "([^"]+)"\nchecksum = "([^"]+)"\n')
wanted = next((m for m in block.finditer(open(our_lock).read().replace('\r\n', '\n')) if m.group(1) == version), None)
if wanted is None:
    sys.exit(f'pin-libc.py: {our_lock} does not name libc {version}')
text = open(std_lock).read()
found = block.search(text)
if found is None:
    sys.exit(f'pin-libc.py: {std_lock} has no libc entry of the expected form')
if found.group(0) != wanted.group(0):
    open(std_lock, 'w').write(text.replace(found.group(0), wanted.group(0), 1))
    print('changed')
