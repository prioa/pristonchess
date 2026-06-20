"""
Pre-build script: Gzip-compress minified web assets and place them
in the data/ directory for LittleFS filesystem upload.

Files with '.nogz.' in the name are copied as-is (no gzip).
All other supported files are gzip-compressed and stored with a .gz suffix.
ESPAsyncWebServer's serveStatic automatically detects .gz files and
serves them with Content-Encoding: gzip.
"""

from pathlib import Path
import shutil
import subprocess
import sys

Import("env")

try:
    import zopfli.gzip as zopfli_gzip
except ImportError:
    print("Installing zopfli package...")
    subprocess.check_call(
        [sys.executable, "-m", "pip", "install", "zopfli", "-q"]
    )
    import zopfli.gzip as zopfli_gzip

BUILD_DIR = Path("src/web/build")
DATA_DIR = Path("data")

SUPPORTED_EXTENSIONS = {".html", ".css", ".js", ".svg", ".mp3", ".m4a", ".png", ".json", ".woff2"}


def is_nogz(filename: str) -> bool:
    return ".nogz." in filename


def prepare():
    # Clean and recreate data directory
    if DATA_DIR.exists():
        shutil.rmtree(DATA_DIR)
    DATA_DIR.mkdir()

    count = 0

    for f in sorted(BUILD_DIR.rglob("*")):
        if not f.is_file():
            continue

        ext = f.suffix.lower()
        if ext not in SUPPORTED_EXTENSIONS:
            continue

        # Get path relative to build dir and clean .nogz. from name
        rel = f.relative_to(BUILD_DIR)
        clean_name = str(rel).replace("\\", "/").replace(".nogz", "")

        if is_nogz(f.name):
            # Binary files that don't benefit from gzip — copy as-is
            out = DATA_DIR / clean_name
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(f, out)
        else:
            # Zopfli compress - max gzip-compatible compression (brotli/zstd aren't accepted over HTTP)
            out = DATA_DIR / (clean_name + ".gz")
            out.parent.mkdir(parents=True, exist_ok=True)
            raw = f.read_bytes()
            compressed = zopfli_gzip.compress(raw, numiterations=100)
            # Force timestamp=0 and OS byte=0x03 (Unix) in gzip header for consistent output across platforms
            compressed = (
                compressed[:4] + b"\x00\x00\x00\x00" +
                compressed[8:9] + b"\x03" + compressed[10:]
            )
            out.write_bytes(compressed)

        count += 1

    # Clean up minified files and copied directories from build/
    for entry in sorted(BUILD_DIR.iterdir(), reverse=True):
        if entry.name.startswith(".") or entry.suffix == ".py":
            continue  # keep .gitignore and build scripts
        if entry.is_dir():
            shutil.rmtree(entry)
        elif entry.is_file():
            entry.unlink()

    print(f"LittleFS: Prepared {count} web assets in data/")


# Only run if minified files exist in build/
has_files = BUILD_DIR.exists() and any(
    f.is_file() and f.suffix.lower() in SUPPORTED_EXTENSIONS
    for f in BUILD_DIR.rglob("*")
)

if not has_files:
    if DATA_DIR.exists() and any(DATA_DIR.rglob("*")):
        print(
            "No minified files in src/web/build/ - using existing data/ directory."
        )
    else:
        print(
            "No files found in data/ - LittleFS image will be empty.",
            file=sys.stderr,
        )
else:
    prepare()
