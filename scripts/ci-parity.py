#!/usr/bin/env python3
"""Compare complete TCG and KVM sharded runs (stdlib only)."""
import json
import math
import pathlib
import re
import sys


TCG_ONLY = frozenset({"scripts/tests/boot-segments.sh"})


class Invalid(Exception):
    pass


def load(path, accelerator):
    try:
        def unique(pairs):
            value = {}
            for key, item in pairs:
                if key in value:
                    raise Invalid(f'duplicate JSON key: {key}')
                value[key] = item
            return value

        with open(path, encoding='utf-8') as stream:
            data = json.load(stream, object_pairs_hook=unique)
    except (OSError, ValueError, OverflowError, RecursionError) as error:
        raise Invalid(f'cannot read results: {path}: {error}') from error
    if not isinstance(data, dict) or data.get('accelerator') != accelerator:
        raise Invalid(f'invalid accelerator: {path}')
    tests = data.get('tests')
    if not isinstance(tests, list) or not tests or data.get('selected') != len(tests):
        raise Invalid(f'incomplete test list: {path}')
    found = {}
    for test in tests:
        if not isinstance(test, dict):
            raise Invalid(f'invalid test: {path}')
        name, result, duration = (test.get(key) for key in ('name', 'result', 'duration'))
        if (not isinstance(name, str) or not name or name in found or
                result not in ('passed', 'skipped', 'failed', 'timed out', 'interrupted', 'not run', 'no result') or
                duration is not None and (isinstance(duration, bool) or
                not isinstance(duration, (int, float)) or not math.isfinite(duration) or duration < 0) or
                result == 'passed' and duration is None):
            raise Invalid(f'invalid or duplicate test: {path}')
        found[name] = (result, duration)
    return found


def escape(text, property=False):
    """Escape workflow-command data; property delimiters are not message delimiters."""
    value = str(text).replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')
    if property:
        value = value.replace(':', '%3A').replace(',', '%2C')
    value = value[:3500]
    return re.sub(r"%(?:[0-9A-F]?)$", "", value)


def markdown(text):
    return ''.join(f'&#{ord(c)};' if c in '&|<>[]!`\\*_~#@\r\n' else c for c in text)


def emit(message, bad=False):
    print(('::warning::' if bad else '::notice::') + escape(message))


def union(directory, accelerator):
    directory = pathlib.Path(directory)
    shards = sorted(directory.glob('results-*/results.json'))
    if not shards:
        raise Invalid(f'no {accelerator} reports: {directory}')
    tests = {}
    for file in shards:
        shard = load(file, accelerator)
        if set(tests) & set(shard):
            raise Invalid(f'duplicate {accelerator} test across shards: {file}')
        tests.update(shard)
    return tests


def main(args):
    if len(args) not in (3, 4):
        raise Invalid('usage: ci-parity.py TCG_ARTIFACTS KVM_ARTIFACTS EXPECTED_TESTS [SUMMARY]')
    callers = {str(path) for path in pathlib.Path("scripts/tests").glob("*.sh")
               if path.name != "accelerator.sh" and re.search(r"^\s*qemu_require_tcg\b", path.read_text(), re.M)}
    if callers != TCG_ONLY:
        raise Invalid(f"TCG-only test list stale: {sorted(callers ^ TCG_ONLY)}")
    tcg = union(args[0], 'tcg')
    kvm = union(args[1], 'kvm')
    expected = pathlib.Path(args[2]).read_text(encoding='utf-8').splitlines()
    if not expected or any(not name or name != name.strip() for name in expected) or len(set(expected)) != len(expected):
        raise Invalid('invalid expected test list')
    if set(tcg) != set(expected) or set(kvm) != set(expected):
        raise Invalid(f'incomplete test coverage: TCG missing {len(set(expected) - set(tcg))}, '
                      f'KVM missing {len(set(expected) - set(kvm))}, '
                      f'extra {len((set(tcg) | set(kvm)) - set(expected))}')
    differences = []
    tcg_only = set()
    for name in sorted(set(tcg) | set(kvm)):
        expected = tcg.get(name)
        actual = kvm.get(name)
        if (name in TCG_ONLY and expected is not None and actual is not None and
                expected[0] == 'passed' and actual[0] == 'skipped'):
            tcg_only.add(name)
            continue
        if expected is None or actual is None or expected[0] != actual[0] or actual[0] != 'passed':
            differences.append(f'{name}: TCG {expected[0] if expected else "missing"}, '
                               f'KVM {actual[0] if actual else "missing"}')
    # The runner has one result per test, not individual check IDs. Compare these
    # result sets, including skipped tests, and reject incomplete/duplicate reports.
    ratios = []
    for name in tcg:
        base = tcg[name][1]
        if tcg[name][0] != 'passed' or not base or base <= 0:
            continue
        item = kvm.get(name)
        if item and item[0] == 'passed' and item[1] is not None:
            ratios.append((item[1] / base, name, item[1], base))
    ratios.sort(reverse=True)
    # Compare identical test sets; TCG-only tests have no KVM duration.
    shared = set(tcg) - TCG_ONLY
    tcg_total = sum(item[1] for name, item in tcg.items() if name in shared and item[0] == "passed")
    kvm_total = sum(item[1] for name, item in kvm.items() if name in shared and item[0] == 'passed')
    totals = f'TCG {tcg_total:.1f}s; KVM {kvm_total:.1f}s'
    top = '; '.join(f'{name}: {ratio:.2f}x ({duration:.1f}s/{base:.1f}s)'
                    for ratio, name, duration, base in ratios[:5])
    lines = ['## KVM / TCG parity', '', f'{len(tcg)} tests; '
             + ('differences: ' + str(len(differences)) if differences else 'parity OK') + '.',
             f'TCG-only: {len(tcg_only)}; {totals}.', '', 'Slowest KVM/TCG ratios (per-test seconds):', '', '| Test | KVM | TCG | Ratio |',
             '| --- | ---: | ---: | ---: |']
    for ratio, name, duration, base in ratios[:10]:
        safe = markdown(name)
        lines.append(f'| {safe[:160]} | {duration:.1f} | {base:.1f} | {ratio:.2f} |')
    if differences:
        lines += ['', 'Differing tests (first 12):']
        lines += ['- ' + markdown(item) for item in differences[:12]]
    summary = '\n'.join(lines) + '\n'
    if len(args) == 4:
        with open(args[3], 'w', encoding='utf-8') as out:
            out.write(summary[:6000])
    print(summary)
    if differences:
        emit(f'KVM/TCG parity differs in {len(differences)} test/run pairs; TCG-only {len(tcg_only)}; {totals}; top 5: {top}; ' +
             '; '.join(differences[:12]) + ('; ...' if len(differences) > 12 else ''), True)
    else:
        emit(f'KVM/TCG parity OK: {len(tcg)} tests, TCG-only {len(tcg_only)}; {totals}; top 5: {top}')
    return int(bool(differences))


if __name__ == '__main__':
    try:
        code = main(sys.argv[1:])
    except (Invalid, OSError, UnicodeError, ValueError, OverflowError, RecursionError) as error:
        emit(f'KVM/TCG parity error: {ascii(str(error))}', True)
        code = 1
    sys.exit(code)
