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

report --run-dir DIR --jobs N --shard I/N|'' --accelerator NAME --elapsed SECONDS [--summary-limit BYTES]
    Writes the run's reports into DIR, from the files run-tests.sh leaves there: plan (the tests, NUL
    after each), instances (number and directory of each test started, NUL after each) and
    <directory>/result (exit status, run time in tenths of a second, and done, timeout or cancel).
    results.json  every test: name, instance, result, exit status, duration, output directory
    junit.xml     the same as JUnit XML
    summary.md    a Markdown table for the CI job summary, at most BYTES bytes (default 1000000)
    failed/       for each test that didn't pass or skip, a copy of its output and its log, text and
                  screen files (not disk images), each file and each test bounded in size; index.txt
                  there says what was cut or left out
"""

import json
import math
import os
import re
import stat
import sys
import xml.etree.ElementTree as ET


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


def split_nul(path):
    with open(path, "rb") as f:
        data = f.read()
    return data.split(b"\0")[:-1] if data else []


def text(raw):
    """Bytes as text that XML can hold: invalid UTF-8 and control characters become '?'."""
    s = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw
    return re.sub("[^\t\n\r\x20-\ud7ff\ue000-\ufffd\U00010000-\U0010ffff]", "?", s)


def markdown(s):
    """A test name as plain text in a Markdown table cell."""
    return "".join(f"&#{ord(c)};" if c in "&|<>[]!`\\*_~#@\r\n" else c for c in text(s))


def tail(path, max_bytes, max_lines):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - max_bytes))
            data = f.read()
    except OSError:
        return ""
    lines = data.split(b"\n")
    cut = size > max_bytes or len(lines) > max_lines + 1
    data = b"\n".join(lines[-max_lines - 1:])
    return ("[... earlier output cut]\n" if cut else "") + text(data)


EVIDENCE = (".log", ".txt", ".out", ".ppm")   # output, debugcon, monitor, screen, debugger logs
FILE_LIMIT = 1 << 20      # a text file keeps its first 64 KiB and its end; a bigger image is left out
TEST_LIMIT = 8 << 20
ALL_LIMIT = 64 << 20


def copy_evidence(src_root, dst_root, budget):
    """Copies a failed test's evidence files; returns the bytes written."""
    notes, used = [], 0
    for top, dirs, files in os.walk(src_root):
        dirs.sort()
        for name in sorted(files):
            src = os.path.join(top, name)
            rel = os.path.relpath(src, src_root)
            try:
                st = os.lstat(src)
            except OSError:
                continue
            if not stat.S_ISREG(st.st_mode):
                notes.append(f"left out (not a regular file): {rel}")
                continue
            if not name.endswith(EVIDENCE):
                if name != "result":
                    notes.append(f"left out ({st.st_size} bytes, not a log, text or screen file): {rel}")
                continue
            room = min(FILE_LIMIT, TEST_LIMIT - used, budget - used)
            if room < 4096:
                notes.append(f"left out ({st.st_size} bytes, the size limit for failed output is reached): {rel}")
                continue
            try:
                fd = os.open(src, os.O_RDONLY | os.O_NOFOLLOW)
                with os.fdopen(fd, "rb") as f:
                    if st.st_size <= room:
                        data = f.read(room)
                    elif name.endswith(".ppm"):
                        notes.append(f"left out ({st.st_size} bytes, an image too big to keep): {rel}")
                        continue
                    else:
                        head = f.read(64 << 10)
                        f.seek(-(room - len(head) - 128), os.SEEK_END)
                        rest = f.read()
                        gone = st.st_size - len(head) - len(rest)
                        data = head + f"\n[... {gone} bytes cut here ...]\n".encode() + rest
                        notes.append(f"cut to {len(data)} of {st.st_size} bytes (start and end kept): {rel}")
            except OSError as e:
                notes.append(f"can't read {rel}: {e.strerror}")
                continue
            dst = os.path.join(dst_root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(data)
            used += len(data)
    with open(os.path.join(dst_root, "index.txt"), "w", encoding="utf-8", errors="replace") as f:
        f.write("Copied: output.log, and the test's log, text and screen files.\n")
        f.write("".join(n + "\n" for n in notes) or "Nothing was cut or left out.\n")
    return used


def cmd_report(args):
    opts = {"--summary-limit": "1000000"}
    names = ("--run-dir", "--jobs", "--shard", "--accelerator", "--elapsed", "--summary-limit")
    while args:
        a = args.pop(0)
        if a not in names or not args:
            raise Usage(f"report: unexpected argument '{a}'")
        opts[a] = args.pop(0)
    if len(opts) != len(names):
        raise Usage("report needs " + " ".join(names[:-1]))
    run_dir, shard, accel = opts["--run-dir"], opts["--shard"], opts["--accelerator"]
    limit = int(opts["--summary-limit"])
    plan = split_nul(os.path.join(run_dir, "plan"))
    started = split_nul(os.path.join(run_dir, "instances"))
    dirs = {int(started[k]): started[k + 1].decode() for k in range(0, len(started) - 1, 2)}
    tests, total = [], 0.0
    for i, path in enumerate(plan):
        d = dirs.get(i + 1)
        if d is not None and ("/" in d or d in ("", ".", "..")):
            raise Usage(f"bad test directory name '{d}'")
        status = duration = None
        if d is None:
            result = "not run"
        else:
            try:
                with open(os.path.join(run_dir, d, "result")) as f:
                    st, tenths, how = f.read().split()
                status, duration = int(st), int(tenths) / 10
                total += duration
                result = {"timeout": "timed out", "cancel": "interrupted"}.get(
                    how, {0: "passed", 77: "skipped"}.get(status, "failed"))
            except (OSError, ValueError):
                result = "no result"
        tests.append({"name": path.decode("utf-8", "surrogateescape"), "instance": f"{i + 1:03d}",
                      "result": result, "exit_status": status, "duration": duration,
                      "output_dir": d, "raw_name": path})
    count = {r: sum(t["result"] == r for t in tests) for r in ("passed", "skipped", "not run")}
    bad = [t for t in tests if t["result"] not in ("passed", "skipped")]
    count["failed"] = len(bad) - count["not run"]

    with open(os.path.join(run_dir, "results.json"), "w") as f:
        json.dump({"shard": shard, "jobs": int(opts["--jobs"]), "accelerator": accel,
                   "selected": len(tests), "passed": count["passed"], "failed": count["failed"],
                   "skipped": count["skipped"], "not_run": count["not run"],
                   "tests": [{k: v for k, v in t.items() if k != "raw_name"} for t in tests]}, f, indent=1)
        f.write("\n")

    suite_name = "RorthOX tests" + (f" (shard {shard})" if shard else "")
    suites = ET.Element("testsuites", name=suite_name)
    suite = ET.SubElement(suites, "testsuite", name=suite_name)
    props = ET.SubElement(suite, "properties")
    for k, v in (("shard", shard), ("jobs", opts["--jobs"]), ("accelerator", accel)):
        ET.SubElement(props, "property", name=k, value=text(v))
    for t in tests:
        case = ET.SubElement(suite, "testcase", name=text(t["raw_name"]), classname="rorthox",
                             time=f"{t['duration'] or 0:.1f}")
        why = "" if t["exit_status"] is None else f" (exit status {t['exit_status']})"
        if t["result"] == "skipped":
            ET.SubElement(case, "skipped", message="skipped" + why)
        elif t["result"] == "not run":
            ET.SubElement(case, "error", message="not run: the run was interrupted")
        elif t["result"] != "passed":
            el = ET.SubElement(case, "failure", message=t["result"] + why, type=t["result"])
            el.text = tail(os.path.join(run_dir, t["output_dir"], "output.log"), 32 << 10, 100)
    for el in (suites, suite):
        el.set("tests", str(len(tests)))
        el.set("failures", str(count["failed"]))
        el.set("errors", str(count["not run"]))
        el.set("skipped", str(count["skipped"]))
        el.set("time", f"{total:.1f}")
    ET.ElementTree(suites).write(os.path.join(run_dir, "junit.xml"), encoding="utf-8", xml_declaration=True)

    elapsed = int(opts["--elapsed"])
    head = [f"### {suite_name}\n", "\n",
            f"{count['passed']} passed, {count['failed']} failed, {count['skipped']} skipped, "
            f"{count['not run']} not run of {len(tests)} in {elapsed // 60} min {elapsed % 60} s "
            f"(up to {opts['--jobs']} at once, shard {shard or 'all'}, accelerator {accel})\n", "\n"]
    rows = ["| Test | Result | Time |\n", "| --- | --- | --- |\n"]
    # Failures first, so the size limit never hides one.
    for t in bad + [t for t in tests if t not in bad]:
        why = "" if t["result"] in ("passed", "skipped") or t["exit_status"] is None \
            else f" (exit status {t['exit_status']})"
        time_s = "" if t["duration"] is None else f"{t['duration']:.1f} s"
        rows.append(f"| {markdown(t['raw_name'])} | {t['result']}{why} | {time_s} |\n")
    out = head + rows[:2]
    size = len("".join(out).encode())
    for k, line in enumerate(rows[2:]):
        note = f"\n{len(rows) - 2 - k} more test(s) not listed here: see results.json.\n"
        if size + len(line.encode()) + len(note.encode()) > limit:
            out.append(note)
            break
        out.append(line)
        size += len(line.encode())
    with open(os.path.join(run_dir, "summary.md"), "w", encoding="utf-8") as f:
        f.write("".join(out))

    budget = ALL_LIMIT
    for t in bad:
        if t["output_dir"] is not None:
            dst = os.path.join(run_dir, "failed", t["output_dir"])
            os.makedirs(dst, exist_ok=True)
            budget -= copy_evidence(os.path.join(run_dir, t["output_dir"]), dst, budget)
    return 0


def main(argv):
    commands = {"plan": cmd_plan, "report": cmd_report}
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
