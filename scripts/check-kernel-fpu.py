#!/usr/bin/env python3
"""Reject floating-point and vector instructions outside the FPU context module."""

import re
import subprocess
import sys

# Operand registers catch SSE/MMX instructions even when their mnemonics are
# shared with integer instructions (for example movd). The second expression
# catches instructions with implicit registers or memory-only operands.
fp_register = re.compile(r"\b(?:st(?:\([0-7]\))?|mm[0-7]|[xyz]mm[0-9]+)\b", re.I)
fp_opcode = re.compile(
    r"^(?:f[a-z0-9]+|emms|femms|fxsave(?:64)?|fxrstor(?:64)?|"
    r"ldmxcsr|stmxcsr|xsave\w*|xrstor\w*|vzero\w*|"
    r"maskmov\w*|prefetch\w*|(?:s|l|m)fence)$", re.I
)
section = re.compile(r"^\s*\d+\s+(\S+)\s+[^\n]*\n[^\n]*\bCODE\b", re.M)


def disassembly(obj):
    header = subprocess.run(
        ["i686-elf-objdump", "-h", obj], check=True, capture_output=True, text=True
    ).stdout
    # Include every CODE section, not just .text (GCC may create .text.*).
    # NASM's .asm is not flagged executable, so find that by name as well.
    sections = set(section.findall(header))
    if re.search(r"^\s*\d+\s+\.asm\s+", header, re.M):
        sections.add(".asm")
    if not sections:
        raise ValueError(f"{obj}: no code or .asm section to inspect")
    for name in sorted(sections):
        mode = "-D" if name == ".asm" else "-d"
        output = subprocess.run(
            ["i686-elf-objdump", mode, "-M", "intel", "-j", name, obj],
            check=True, capture_output=True, text=True
        ).stdout
        for line in output.splitlines():
            # objdump emits address, bytes, mnemonic/operands as tab-separated
            # fields, not just mnemonic at the beginning of each line.
            fields = line.split("\t")
            if len(fields) < 3 or not re.match(r"^\s*[0-9a-f]+:$", fields[0]):
                continue
            instruction = fields[2].split("#", 1)[0].strip()
            if instruction:
                yield name, line, instruction


def main(objects):
    if not objects:
        raise ValueError("pass the complete kernel object list")
    allowed = "build/arch/fpu.asm.o"
    violations = []
    for obj in objects:
        if obj.removeprefix("./") == allowed:
            continue
        for name, line, instruction in disassembly(obj):
            opcode = instruction.split()[0]
            if fp_register.search(instruction) or fp_opcode.fullmatch(opcode):
                violations.append(f"{obj} ({name}): {line.strip()}")
    if violations:
        raise ValueError("floating-point/vector instructions outside fpu.asm:\n" +
                         "\n".join(violations))
    print(f"kernel FPU scan: {len(objects)} objects checked")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"error: {error}")
