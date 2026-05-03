#!/usr/bin/env python3
"""
session_summary.py — Render a markdown report of the current refactor session.

Walks `git log` from the first session commit (SESSION_START_SHA below)
to HEAD, groups commits by conventional-commit prefix, and writes
SESSION_SUMMARY.md at the repo root.

Usage:
    python3 scripts/session_summary.py            # writes SESSION_SUMMARY.md
    python3 scripts/session_summary.py --stdout   # prints to stdout
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Tuple

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# First commit of this session (inclusive). Update if you start a new session.
SESSION_START_SHA = "46ea0dd"

REPO_URL = "https://github.com/kanar11/hft-infra-lab"

# Conventional-commit prefix → human-readable group label, in display order.
GROUPS: List[Tuple[str, str]] = [
    ("feat",       "Features"),
    ("perf",       "Performance"),
    ("refactor",   "Refactor"),
    ("fix",        "Fixes"),
    ("hardening",  "Hardening"),
    ("test",       "Tests"),
    ("ci",         "CI / tooling"),
    ("chore",      "Chores"),
]

# Phases described inline in the session — referenced by the summary preamble.
PHASE_LABELS = [
    ("Phase 1", "Risk Manager pending exposure"),
    ("Phase 2", "Realistic simulator fill latency"),
    ("Phase 3", "Trade Logger hot-path review"),
    ("Phase 4", "SPSC queue audit + stress test"),
    ("Phase 5", "Tighten cppcheck suppressions"),
    ("Phase 6", "Strategy edge cases"),
    ("Phase 7", "Parser robustness"),
    ("Round R1", "O(1) perf hotspots"),
    ("Round R2", "common/ shared utilities"),
    ("Round R3", "CI: matrix + sanitizers"),
    ("Round R4", "Test coverage gaps"),
    ("Round R5", "Silent-failure warnings"),
]


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------

def git(*args: str) -> str:
    out = subprocess.run(["git", *args], capture_output=True, text=True, check=True)
    return out.stdout


def session_commits() -> List[Tuple[str, str]]:
    """Return [(short_sha, subject), ...] from SESSION_START_SHA^..HEAD, oldest first."""
    raw = git("log", "--reverse", "--pretty=%h\t%s", f"{SESSION_START_SHA}^..HEAD")
    rows = [line.split("\t", 1) for line in raw.splitlines() if line.strip()]
    return [(sha, subj) for sha, subj in rows]


def group_for(subject: str) -> str:
    """Pick the bucket for a commit subject by its conventional-commit prefix."""
    m = re.match(r"^(\w+)(?:\([^)]+\))?:", subject)
    if not m:
        return "Other"
    prefix = m.group(1)
    for tag, label in GROUPS:
        if prefix == tag:
            return label
    return "Other"


# ---------------------------------------------------------------------------
# Render
# ---------------------------------------------------------------------------

def render(commits: List[Tuple[str, str]]) -> str:
    buckets: Dict[str, List[Tuple[str, str]]] = {label: [] for _, label in GROUPS}
    buckets["Other"] = []
    for sha, subj in commits:
        buckets[group_for(subj)].append((sha, subj))

    lines: List[str] = []
    lines.append("# HFT Infra Lab — Refactor Session Summary")
    lines.append("")
    lines.append(f"`{SESSION_START_SHA}` → `{commits[-1][0]}` &mdash; "
                 f"**{len(commits)} commits** across 7 planned phases plus 5 follow-up rounds.")
    lines.append("")
    lines.append("## What got done")
    lines.append("")
    for tag, label in PHASE_LABELS:
        lines.append(f"- **{tag}** — {label}")
    lines.append("")
    lines.append("## Commits by category")
    lines.append("")
    for _, label in GROUPS:
        items = buckets[label]
        if not items:
            continue
        lines.append(f"### {label} ({len(items)})")
        lines.append("")
        for sha, subj in items:
            lines.append(f"- [`{sha}`]({REPO_URL}/commit/{sha}) {subj}")
        lines.append("")
    if buckets["Other"]:
        lines.append(f"### Other ({len(buckets['Other'])})")
        lines.append("")
        for sha, subj in buckets["Other"]:
            lines.append(f"- [`{sha}`]({REPO_URL}/commit/{sha}) {subj}")
        lines.append("")

    lines.append("## Known follow-ups")
    lines.append("")
    lines.append("- Bilingual (PL+ENG) comment style is inconsistent across modules — "
                 "ad-hoc cleanup, not blocking.")
    lines.append("- `cppcheck-suppress uninitvar` on `SPSCQueue<T,SIZE>{}` "
                 "in `tests/test_all.cpp` is a known cppcheck template-modeling "
                 "limitation, intentionally kept.")
    lines.append("- CI matrix could be extended with macOS / `-fsanitize=memory` "
                 "/ `clang-tidy` once we want stricter coverage.")
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stdout", action="store_true", help="print to stdout instead of writing file")
    ap.add_argument("--out", default="SESSION_SUMMARY.md", help="output filename (default: SESSION_SUMMARY.md)")
    args = ap.parse_args()

    commits = session_commits()
    if not commits:
        print(f"No commits found from {SESSION_START_SHA} onward.", file=sys.stderr)
        return 1

    md = render(commits)

    if args.stdout:
        print(md)
    else:
        repo_root = Path(git("rev-parse", "--show-toplevel").strip())
        target = repo_root / args.out
        target.write_text(md, encoding="utf-8")
        print(f"Wrote {target} ({len(md)} bytes, {len(commits)} commits)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
