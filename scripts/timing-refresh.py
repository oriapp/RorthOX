#!/usr/bin/env python3
"""Make a candidate scripts/test-timings.json from serial TCG runs (standard library only).

Usage: timing-refresh.py --current FILE --out FILE RESULTS.json...

Each RESULTS.json is the results.json of one whole-suite run of scripts/run-tests.sh under TCG with
-j 1 and no --shard. A test's new run time is the median of its passed runs, in tenths of a second
(an even count takes the mean of the middle two, a half tenth rounding up), at least 0.1 seconds.
Failed, skipped, timed-out, interrupted, not-run and missing runs never count, so a timeout never
becomes a run time. A test with no passed run keeps its time from --current; a test that is in
neither file stays out (run-tests.sh gives it the median). The same inputs always give the same
bytes: sorted keys, two-space indent, one decimal. It prints a short summary of what changed.

Exit status 0 with the candidate written to --out; 2 for a bad command line or a bad or
incomplete input file. --out is removed first, so after a failure there is no candidate.
"""

import json
import math
import os
import runpy
import sys

ABOUT = ("Seconds per test for run-tests.sh --shard: the median of the passed runs of each test in "
         "serial runs (-j 1) of the whole suite under TCG. Tests not listed here count as the median.")
RESULTS = ("passed", "skipped", "failed", "timed out", "interrupted", "not run", "no result")
TOP = 5


class Invalid(Exception):
    pass


def load_results(path):
    """{test name: tenths of a second or None when the run didn't pass} from one results.json."""
    def unique(pairs):
        seen = {}
        for key, value in pairs:
            if key in seen:
                raise Invalid(f'{path!a}: duplicate key {key!a}')
            seen[key] = value
        return seen

    def no_constant(name):
        raise Invalid(f'{path!a}: {name!a} is not a number')

    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f, object_pairs_hook=unique, parse_constant=no_constant)
    except (OSError, ValueError, OverflowError, RecursionError) as e:
        raise Invalid(f"can't read results {path!a}: {type(e).__name__}") from e
    if not isinstance(data, dict):
        raise Invalid(f'{path!a}: not a results object')
    if data.get("accelerator") != "tcg" or data.get("jobs") != 1 or data.get("shard") != "":
        raise Invalid(f'{path!a}: not a serial whole-suite TCG run (accelerator tcg, jobs 1, no shard)')
    tests = data.get("tests")
    if not isinstance(tests, list) or not tests or data.get("selected") != len(tests):
        raise Invalid(f'{path!a}: incomplete test list')
    runs = {}
    for test in tests:
        if not isinstance(test, dict):
            raise Invalid(f'{path!a}: a test entry is not an object')
        name, result, duration = (test.get(key) for key in ("name", "result", "duration"))
        if not isinstance(name, str) or not name or name in runs:
            raise Invalid(f'{path!a}: missing or duplicate test name {name!a}')
        if result not in RESULTS:
            raise Invalid(f'{path!a}: {name!a} has unknown result {result!a}')
        if duration is not None and (isinstance(duration, bool) or not isinstance(duration, (int, float))
                                     or not math.isfinite(duration) or duration < 0):
            raise Invalid(f'{path!a}: {name!a} has a bad duration {duration!a}')
        if result == "passed" and duration is None:
            raise Invalid(f'{path!a}: {name!a} passed without a duration')
        runs[name] = round(duration * 10) if result == "passed" else None
    return runs


def median_tenths(values):
    values = sorted(values)
    mid = len(values) // 2
    value = values[mid] if len(values) % 2 else (values[mid - 1] + values[mid] + 1) // 2
    return max(value, 1)


def printable(name):
    return "".join(c if " " <= c != "\x7f" else "?" for c in name)


def refresh(current_file, result_files):
    """(candidate timings, summary lines)."""
    here = os.path.dirname(os.path.realpath(__file__))
    runner = runpy.run_path(os.path.join(here, "run-tests.py"))
    try:
        old = runner["load_timings"](current_file)
    except runner["Usage"] as e:
        raise Invalid(f"bad --current: {e}") from e
    samples = {}
    for path in result_files:
        for name, tenths in load_results(path).items():
            samples.setdefault(name, [])
            if tenths is not None:
                samples[name].append(tenths)
    new = dict(old)
    measured = {name: median_tenths(values) / 10 for name, values in samples.items() if values}
    new.update(measured)
    kept = sorted(name for name in old if name not in measured)
    added = sorted(name for name in measured if name not in old)
    unmeasured = sorted(name for name, values in samples.items() if not values and name not in old)
    moves = sorted(((abs(measured[n] - old[n]), n) for n in measured if n in old and measured[n] != old[n]),
                   key=lambda m: (-m[0], m[1]))
    lines = [f"timing refresh: {len(result_files)} run(s), {len(measured)} test(s) measured, "
             f"{len(moves)} changed, {len(added)} new, {len(kept)} kept without a passed run, "
             f"{len(unmeasured)} new without a passed run (median)"]
    for _, name in moves[:TOP]:
        lines.append(f"  {printable(name)}: {old[name]:.1f} -> {measured[name]:.1f} s "
                     f"({measured[name] - old[name]:+.1f})")
    for name in added[:TOP]:
        lines.append(f"  {printable(name)}: new, {measured[name]:.1f} s")
    for name in kept[:TOP]:
        lines.append(f"  {printable(name)}: kept {old[name]:.1f} s (no passed run)")
    return new, lines


def main(argv):
    current = out = None
    files = []
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--current" and args and current is None:
            current = args.pop(0)
        elif a == "--out" and args and out is None:
            out = args.pop(0)
        elif a.startswith("--"):
            raise Invalid(f"unexpected argument {a!a}")
        else:
            files.append(a)
    if out is None:
        raise Invalid("usage: timing-refresh.py --current FILE --out FILE RESULTS.json...")
    inputs = [current] if current is not None else []
    if os.path.realpath(out) in map(os.path.realpath, inputs + files):
        raise Invalid("--out must not be an input file")
    try:
        os.unlink(out)
    except FileNotFoundError:
        pass
    except OSError as e:
        raise Invalid(f"can't remove the old --out: {type(e).__name__}") from e
    if current is None or not files:
        raise Invalid("usage: timing-refresh.py --current FILE --out FILE RESULTS.json...")
    timings, lines = refresh(current, files)
    text = json.dumps({"about": ABOUT, "tests": timings}, indent=2, sort_keys=True) + "\n"
    temp = out + ".tmp"
    try:
        with open(temp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(temp, out)
    except OSError as e:
        try:
            os.unlink(temp)
        except OSError:
            pass
        raise Invalid(f"can't write --out: {type(e).__name__}") from e
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (Invalid, OverflowError, RecursionError) as e:
        print(f"timing-refresh: {e}", file=sys.stderr)
        sys.exit(2)
