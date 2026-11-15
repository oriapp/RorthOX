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
section = re.compile(r"^\s*\d+\s+(\S+)\s+[^\n]*\n\s*([A-Z, ]+)$", re.M)
# NASM's default .asm section has ALLOC but no executable flag. Other
# allocated contents with an unexpected name must not hide instructions.
data_section = re.compile(r"^(?:\.data(?:\..*)?|\.rodata(?:\..*)?|\.bss(?:\..*)?)$")


def disassembly(obj):
    header = subprocess.run(
        ["i686-elf-objdump", "-h", obj], check=True, capture_output=True, text=True
    ).stdout
    sections = set()
    for name, flag_text in section.findall(header):
        flags = set(flag_text.split(", "))
        # CODE corresponds to ELF SHF_EXECINSTR; include all executable
        # sections, regardless of name or where they occur in the object.
        if "CODE" in flags or name == ".asm":
            sections.add(name)
        elif {"ALLOC", "CONTENTS"} <= flags and not data_section.fullmatch(name):
            raise ValueError(f"{obj}: unscanned allocated section {name}")
    if not sections:
        raise ValueError(f"{obj}: no executable or .asm section to inspect")
    for name in sorted(sections):
        output = subprocess.run(
            ["i686-elf-objdump", "-D", "-M", "intel", "-j", name, obj],
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
        in_fpu = obj.removeprefix("./") == allowed
        for name, line, instruction in disassembly(obj):
            opcode = instruction.split()[0].lower()
            if fp_register.search(instruction) or fp_opcode.fullmatch(opcode):
                if not in_fpu or opcode not in {"fxsave", "fxrstor", "fninit"}:
                    violations.append(f"{obj} ({name}): {line.strip()}")
    if violations:
        raise ValueError("disallowed floating-point/vector instructions:\n" +
                         "\n".join(violations))
    print(f"kernel FPU scan: {len(objects)} objects checked")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"error: {error}")
