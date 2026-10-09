#!/bin/bash
# Renders the same screens with the UI as it was at a commit and as it is now, and counts
# the pixels that differ: a check that a change meant to leave the television UI alone has.
#   compare.sh <commit>
# The earlier version is checked out and built in build/before (a git worktree); the
# pictures go to build/shots/cmp. See ../MOCKUPS.md.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
# (Named as this checkout sees it: "HEAD" means this one's, not the worktree's.)
commit=$(git -C "$repo" rev-parse "$1")
before=$repo/build/before
if [[ ! -d $before ]]; then
    git -C "$repo" worktree add --detach "$before" "$commit" >/dev/null 2>&1
else
    git -C "$before" checkout -q --detach "$commit"
fi
# (The preview's build script writes its log beside its build folder, which has to exist.)
mkdir -p "$before/build"
bash "$before/shared/ui/tools/preview.sh" 2>&1 | grep -i "error" || true
bash "$repo/shared/ui/tools/preview.sh" 2>&1 | grep -i "error" || true
data=$repo/build/preview-data
mkdir -p "$repo/build/shots/cmp"
names=(board board-moved title series discover calendar addons settings search list)
keys=("" "rrdd" "a" "dda" "Sldarrd" "Sldddar" "Slddddadd" "Slddddddaddd" "uadr" "ldaurradd")
for index in "${!names[@]}"; do
    for side in before now; do
        exe=$repo/build/preview/stremio_preview.exe
        [[ $side == before ]] && exe=$before/build/preview/stremio_preview.exe
        "$exe" "$data/board.json" "$data/images" "$repo/shared/ui/assets/fonts" \
            --shot "$repo/build/shots/cmp/${names[$index]}-$side.png" --keys "${keys[$index]}" >/dev/null 2>&1
    done
done
python - "$repo/build/shots/cmp" "${names[@]}" <<'EOF'
import sys, zlib, struct
def pixels(path):
    data = open(path, 'rb').read()
    pos, chunks, width, height = 8, b'', 0, 0
    while pos < len(data):
        size, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + size]
        if kind == b'IHDR': width, height = struct.unpack('>II', body[:8])
        if kind == b'IDAT': chunks += body
        pos += 12 + size
    raw = zlib.decompress(chunks)
    stride = width * 4
    rows, previous = [], bytearray(stride)
    for y in range(height):
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        kind = raw[y * (stride + 1)]
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = previous[x]
            c = previous[x - 4] if x >= 4 else 0
            if kind == 1: line[x] = (line[x] + a) & 255
            elif kind == 2: line[x] = (line[x] + b) & 255
            elif kind == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line)); previous = line
    return b''.join(rows)
folder = sys.argv[1]
for name in sys.argv[2:]:
    one, two = pixels(f'{folder}/{name}-before.png'), pixels(f'{folder}/{name}-now.png')
    differ = sum(1 for i in range(0, len(one), 4) if one[i:i + 3] != two[i:i + 3]) if one != two else 0
    print(f'{name}: {differ} pixels differ')
EOF
