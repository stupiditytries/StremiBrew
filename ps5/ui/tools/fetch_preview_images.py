"""Fills the PC preview's image cache with the posters the sample board refers to.

    python fetch_preview_images.py <board.json> <cache folder> [items per row]

The app keeps downloaded images in a folder, one file per image, named by a hash of the
image's address (see image_cache_name in src/images.cpp). On the console the app fills that
folder itself; for the preview this script does.
"""
import json
import pathlib
import sys
import urllib.request


def cache_name(url: str) -> str:
    """64-bit FNV-1a of the address, as 16 hex digits."""
    value = 0xCBF29CE484222325
    for byte in url.encode("utf-8"):
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}.img"


def main() -> None:
    board = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
    cache = pathlib.Path(sys.argv[2])
    per_row = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    cache.mkdir(parents=True, exist_ok=True)
    fetched = skipped = failed = 0
    for row in board:
        for item in row["items"][:per_row]:
            for url in (item.get("poster"), item.get("background"), item.get("logo")):
                if not url:
                    continue
                target = cache / cache_name(url)
                if target.exists():
                    skipped += 1
                    continue
                try:
                    request = urllib.request.Request(
                        url, headers={"User-Agent": "stremio-ps5-preview"})
                    with urllib.request.urlopen(request, timeout=20) as response:
                        target.write_bytes(response.read())
                    fetched += 1
                except OSError as error:
                    failed += 1
                    print(f"failed: {url}: {error}")
    print(f"fetched {fetched}, already present {skipped}, failed {failed}")


if __name__ == "__main__":
    main()
