#!/usr/bin/env python3
"""Helper for scripts/run-tests.sh (run as python3 -B, standard library only).

plan --timings FILE [--shard I/N] -- TEST...
    Checks the run times in FILE and prints the tests to run, each followed by a NUL byte. Without
    --shard that is every test given, in the given order. With --shard I/N the tests are split into
    N shards: the measured tests from the longest to the shortest (path, then position, breaks a
    tie), then the tests FILE doesn't know (by path) with the median run time as their weight, each
    to the shard with the least work so far (the lower shard number breaks a tie). Shard I's tests
    are printed in that order, so its longest tests start first. A test named twice is two tests.
    Exits 2 on a bad shard or a bad timings file.
"""

import json
import math
import os
import re
import sys


class Usage(Exception):
    pass


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.realpath(__file__)))


def test_key(path, root):
    """The name a test has in the timings file: its path from the top of the repository."""
    real = os.path.realpath(path)
    rel = os.path.relpath(real, root)
    return real if rel == os.pardir or rel.startswith(os.pardir + os.sep) else rel


def load_timings(path):
    def no_duplicates(pairs):
        seen = {}
        for key, value in pairs:
            if key in seen:
                raise Usage(f"{path}: '{key}' is there twice")
            seen[key] = value
        return seen

    def no_constant(name):
        raise Usage(f"{path}: {name} is not a number of seconds")

    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f, object_pairs_hook=no_duplicates, parse_constant=no_constant)
    except OSError as e:
        raise Usage(f"can't read the timings file: {e}")
    except ValueError as e:
        raise Usage(f"{path} is not valid JSON: {e}")
    if not isinstance(data, dict) or not isinstance(data.get("tests"), dict):
        raise Usage(f'{path}: expected {{"tests": {{"<path>": <seconds>, ...}}}}')
    timings = {}
    for key, value in data["tests"].items():
        if isinstance(value, bool) or not isinstance(value, (int, float)) \
                or not math.isfinite(value) or value <= 0:
            raise Usage(f"{path}: '{key}' needs a run time above 0 seconds, not {json.dumps(value)}")
        timings[key] = float(value)
    return timings


def parse_shard(text):
    m = re.fullmatch(r"([1-9][0-9]{0,3})/([1-9][0-9]{0,3})", text)
    if not m or int(m.group(1)) > int(m.group(2)):
        raise Usage(f"--shard needs I/N with 1 <= I <= N <= 9999, not '{text}'")
    return int(m.group(1)), int(m.group(2))


def median(values):
    values = sorted(values)
    if not values:
        return 1.0
    mid = len(values) // 2
    return values[mid] if len(values) % 2 else (values[mid - 1] + values[mid]) / 2


def assign(tests, timings, shards, root):
    """Indexes of the tests in each shard, in the order they were assigned."""
    keys = [test_key(t, root) for t in tests]
    default = median(timings.values())
    known = sorted((i for i, k in enumerate(keys) if k in timings),
                   key=lambda i: (-timings[keys[i]], keys[i], i))
    unknown = sorted((i for i, k in enumerate(keys) if k not in timings), key=lambda i: (keys[i], i))
    load = [0.0] * shards
    out = [[] for _ in range(shards)]
    for i in known + unknown:
        s = min(range(shards), key=lambda n: (load[n], n))
        load[s] += timings.get(keys[i], default)
        out[s].append(i)
    return out


def cmd_plan(args):
    timings_file, shard, tests = None, None, None
    while args:
        a = args.pop(0)
        if a == "--timings" and args:
            timings_file = args.pop(0)
        elif a == "--shard" and args:
            shard = parse_shard(args.pop(0))
        elif a == "--":
            tests = args
            break
        else:
            raise Usage(f"plan: unexpected argument '{a}'")
    if timings_file is None or tests is None:
        raise Usage("plan needs --timings FILE and -- TEST...")
    timings = load_timings(timings_file)
    if shard is None:
        chosen = tests
    else:
        chosen = [tests[i] for i in assign(tests, timings, shard[1], repo_root())[shard[0] - 1]]
    out = sys.stdout.buffer
    for t in chosen:
        out.write(os.fsencode(t) + b"\0")
    return 0


def main(argv):
    commands = {"plan": cmd_plan}
    if len(argv) < 2 or argv[1] not in commands:
        print(f"usage: {argv[0]} {'|'.join(commands)} ...", file=sys.stderr)
        return 2
    try:
        return commands[argv[1]](argv[2:])
    except Usage as e:
        print(f"run-tests: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
