"""Make Gateway's flat modules importable from both gateway/ and repository root."""

import sys
from pathlib import Path


GATEWAY_ROOT = str(Path(__file__).resolve().parents[1])
if GATEWAY_ROOT not in sys.path:
    sys.path.insert(0, GATEWAY_ROOT)
