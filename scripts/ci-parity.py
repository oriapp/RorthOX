#!/usr/bin/env python3
"""Compare complete TCG shards with three full KVM runs (stdlib only)."""
import json
import math
import pathlib
import sys


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
    return value[:3500]


def markdown(text):
    return ''.join(f'&#{ord(c)};' if c in '&|<>[]!`\\*_~#@\r\n' else c for c in text)


def emit(message, bad=False):
    print(('::warning::' if bad else '::notice::') + escape(message))


def main(args):
    if len(args) not in (4, 5):
        raise Invalid('usage: ci-parity.py TCG_ARTIFACTS KVM_1 KVM_2 KVM_3 [SUMMARY]')
    shards = []
    tcg = {}
    for i in range(1, 5):
        file = pathlib.Path(args[0]) / f'results-{i}' / 'results.json'
        shard = load(file, 'tcg')
        if set(tcg) & set(shard):
            raise Invalid(f'duplicate TCG test across shards: {file}')
        tcg.update(shard)
        shards.append(shard)
    runs = [load(pathlib.Path(path) / 'results.json', 'kvm') for path in args[1:4]]
    differences = []
    for number, run in enumerate(runs, 1):
        for name in sorted(set(tcg) | set(run)):
            expected = tcg.get(name)
            actual = run.get(name)
            if expected is None or actual is None or expected[0] != actual[0] or actual[0] not in ('passed', 'skipped'):
                differences.append(f'KVM {number} {name}: TCG {expected[0] if expected else "missing"}, '
                                   f'KVM {actual[0] if actual else "missing"}')
    # The runner has one result per test, not individual check IDs. Compare these
    # result sets, including skipped tests, and reject incomplete/duplicate reports.
    ratios = []
    for name in tcg:
        base = tcg[name][1]
        if tcg[name][0] != 'passed' or not base or base <= 0:
            continue
        for number, run in enumerate(runs, 1):
            item = run.get(name)
            if item and item[0] == 'passed' and item[1] is not None:
                ratios.append((item[1] / base, name, number, item[1], base))
    ratios.sort(reverse=True)
    lines = ['## KVM / TCG parity', '', f'{len(tcg)} tests; '
             + ('differences: ' + str(len(differences)) if differences else 'parity OK') + '.',
             '', 'Slowest KVM/TCG ratios (per-test seconds):', '', '| Test | KVM run | KVM | TCG | Ratio |',
             '| --- | ---: | ---: | ---: | ---: |']
    for ratio, name, number, kvm, base in ratios[:10]:
        safe = markdown(name)
        lines.append(f'| {safe[:160]} | {number} | {kvm:.1f} | {base:.1f} | {ratio:.2f} |')
    if differences:
        lines += ['', 'Differing tests (first 12):']
        lines += ['- ' + markdown(item) for item in differences[:12]]
    summary = '\n'.join(lines) + '\n'
    if len(args) == 5:
        with open(args[4], 'w', encoding='utf-8') as out:
            out.write(summary[:6000])
    print(summary)
    if differences:
        emit(f'KVM/TCG parity differs in {len(differences)} test/run pairs: ' +
             '; '.join(differences[:12]) + ('; ...' if len(differences) > 12 else ''), True)
    else:
        emit(f'KVM/TCG parity OK: {len(tcg)} test results, three full KVM runs vs four TCG shards')
    return int(bool(differences))


if __name__ == '__main__':
    try:
        code = main(sys.argv[1:])
    except (Invalid, OSError, UnicodeError, ValueError, OverflowError, RecursionError) as error:
        emit(f'KVM/TCG parity error: {ascii(str(error))}', True)
        code = 1
    sys.exit(code)
