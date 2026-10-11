#!/bin/bash
# Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3.
#
# Determinism guard for the decision cores (Tier 1 of the headless-sim plan).
#
# The capture->replay net only works if DC decisions are pure, deterministic
# functions of their observation: a replayed fixture must decide identically
# forever. RNG in the decision path would make a captured verdict unreproducible.
# This fails the build if any randomness primitive appears under the DC source
# tree, so a future change can't silently make a decision non-replayable.
#
# Run from anywhere:  bash tools/check_determinism.sh
#
# Comments are exempt; code and string literals are not. The guard used to match
# raw bytes, which meant the comment explaining why a file uses a seeded PRNG
# *instead of* urand() failed the very check it was documenting — the sole
# purpose of these names in prose is to say "not this", so the guard was
# punishing the explanation and rewarding silence. A comment cannot call
# anything, so it cannot make a decision non-replayable, and exempting comments
# costs the guard no real coverage. String literals stay in scope: a string is
# emitted at runtime and can carry a config token or a command name.
#
# The stripper is checked against fixtures on every run (--self-test to run only
# those), because a guard that silently stops matching is worse than no guard.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)/src"

exec python3 - "${SRC_DIR}" "${1:-}" <<'PY'
import os
import re
import sys

SRC_DIR = sys.argv[1]
MODE = sys.argv[2] if len(sys.argv) > 2 else ""

# Randomness primitives that must not appear in the decision path. urand/irand/
# frand/rand32/rand_chance/roll_chance are the project helpers; std::rand and
# <random> are the stdlib ones.
PATTERN = re.compile(
    r"\b(?:urand|irand|frand|rand32|rand_chance|roll_chance_[fi]|std::rand)\b"
    r"|#include\s*<random>"
)

# Reviewed allowlist: src/TestRun is the test-run harness, not the decision
# path. Its only live RNG use is rolling a run seed (recorded in the run record
# for exact replay via `.dc test start <d> seed=N`), which preserves
# replayability; comp selection itself uses a pure seeded PRNG.
EXCLUDE_DIRS = {"TestRun"}


def strip_comments(text):
    """Blank out comment bodies, preserving every byte offset and newline.

    Line numbers and columns therefore survive, so a hit still reports the
    place it was found. String and char literals are walked but left intact:
    they stay in scope for the guard, and walking them is what stops a `//`
    inside a string from swallowing the rest of the line.
    """
    out = list(text)
    n = len(text)
    state = "code"
    i = 0
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == "code":
            if c == "/" and nxt == "/":
                state, out[i], out[i + 1] = "line", " ", " "
                i += 2
                continue
            if c == "/" and nxt == "*":
                state, out[i], out[i + 1] = "block", " ", " "
                i += 2
                continue
            if c == '"':
                state = "string"
            elif c == "'" and not (i and (text[i - 1].isalnum() or text[i - 1] == "_")):
                # Not a digit separator (1'000'000), so a real char literal.
                state = "char"
            i += 1
            continue

        if state in ("string", "char"):
            if c == "\\":
                i += 2
                continue
            if c == "\n" or (c == '"' and state == "string") or (c == "'" and state == "char"):
                # A newline closes an unterminated literal rather than eating
                # the rest of the file.
                state = "code"
            i += 1
            continue

        if state == "line":
            if c == "\n":
                j = i - 1
                while j >= 0 and text[j] in " \t\r":
                    j -= 1
                # A line comment ending in a backslash continues onto the next.
                if j < 0 or text[j] != "\\":
                    state = "code"
                i += 1
                continue
            out[i] = " "
            i += 1
            continue

        # block comment
        if c == "*" and nxt == "/":
            out[i], out[i + 1] = " ", " "
            state = "code"
            i += 2
            continue
        if c != "\n":
            out[i] = " "
        i += 1

    return "".join(out)


def hits_in(text):
    return [
        idx
        for idx, line in enumerate(strip_comments(text).splitlines(), 1)
        if PATTERN.search(line)
    ]


# Fixtures for the stripper itself. A guard that stops matching is worse than
# no guard, so these run before every scan.
SELF_TEST = [
    ("bare call", "urand(1, 2);\n", [1]),
    ("line comment naming it", "// urand() is what this file avoids\n", []),
    ("block comment naming it", "/* rand32()\n   irand() */\nint ok = 0;\n", []),
    ("trailing comment after code", "int ok = 0;  // frand() not used\n", []),
    ("// inside a string", 'const char* s = "// x"; urand(3);\n', [1]),
    ("string literal stays in scope", 'const char* s = "urand";\n', [1]),
    ("stdlib header", "#include <random>\n", [1]),
    ("line number survives a comment", "int a = 1;\n// frand()\nstd::rand();\n", [3]),
    ("backslash-continued comment", "// keep \\\n   urand() still comment\nirand();\n", [3]),
    ("apostrophe in code", "int n = 1'000; urand(2);\n", [1]),
    ("substring is not a hit", "int myrand32ish = 0; frandom_thing();\n", []),
]

for name, src, expected in SELF_TEST:
    got = hits_in(src)
    if got != expected:
        print("ERROR: determinism guard self-test failed: %s" % name)
        print("       expected hits on lines %s, got %s" % (expected, got))
        print("       the comment stripper is wrong; fix it before trusting this guard.")
        sys.exit(2)

if MODE == "--self-test":
    print("determinism check: self-test OK (%d fixtures)" % len(SELF_TEST))
    sys.exit(0)

failures = []
for root, dirs, files in os.walk(SRC_DIR):
    dirs[:] = sorted(d for d in dirs if d not in EXCLUDE_DIRS)
    for name in sorted(files):
        if not name.endswith((".cpp", ".h")):
            continue
        path = os.path.join(root, name)
        with open(path, encoding="utf-8", errors="replace") as handle:
            raw = handle.read()
        raw_lines = raw.splitlines()
        for line_no in hits_in(raw):
            failures.append((path, line_no, raw_lines[line_no - 1].strip()))

if failures:
    print("ERROR: randomness primitive found in the DungeonClear source tree.")
    print("       Decisions must stay deterministic so capture->replay fixtures hold.")
    print("       (Comments are exempt: say what you like about urand() in prose.)")
    for path, line_no, text in failures:
        print("%s:%d:%s" % (path, line_no, text))
    sys.exit(1)

print(
    "determinism check: OK (no RNG primitives in code under %s; "
    "comments exempt, TestRun exempt)" % SRC_DIR
)
PY
