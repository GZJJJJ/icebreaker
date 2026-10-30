"""Validate the dedicated Gateway interpreter and pinned runtime ranges."""

from importlib.metadata import PackageNotFoundError, version
import re
import sys


REQUIREMENTS = {
    "websockets": ((14, 0), (16, 0)), "httpx": ((0, 27), (1, 0)),
    "pytest": ((8, 0), (9, 0)), "pytest-asyncio": ((0, 24), (1, 0)),
    "edge-tts": ((7, 0), (8, 0)), "imageio-ffmpeg": ((0, 5), (1, 0)),
}


def main() -> int:
    failures = []
    if sys.version_info[:2] != (3, 12):
        failures.append(f"requires Python 3.12; found {sys.version.split()[0]}")
    for package, (minimum, exclusive_maximum) in REQUIREMENTS.items():
        try:
            installed = version(package)
        except PackageNotFoundError:
            failures.append(f"{package} is not installed")
            continue
        match = re.match(r"^(\d+)\.(\d+)(?:\.(\d+))?", installed)
        found = tuple(int(part or 0) for part in match.groups()) if match else ()
        if not minimum <= found[:2] < exclusive_maximum:
            failures.append(f"{package}=={installed} is outside [{'.'.join(map(str, minimum))}, {'.'.join(map(str, exclusive_maximum))})")
    if failures:
        print("Gateway environment check failed:", file=sys.stderr)
        print("\n".join(f"- {failure}" for failure in failures), file=sys.stderr)
        return 1
    print(f"Gateway environment OK: {sys.executable} ({sys.version.split()[0]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
