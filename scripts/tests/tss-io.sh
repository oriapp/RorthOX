#!/usr/bin/env bash
#
# Ring 3 must not reach I/O ports. User programs run with IOPL 0, so the CPU checks the I/O
# permission bitmap of the TSS, and the kernel's TSS has none: its I/O map base (104) points past
# the TSS limit (0x67). "in" and "out" from a user program must raise #GP (error 0) at that
# instruction and end only that program. With a 104-byte TSS and an I/O map base of 0, the CPU
# would read TSS fields as the bitmap and let port 0x80 through. With a base inside the TSS (say
# 0x60), the zero bytes from there to the limit would let low ports through, the PIC at
# 0x20/0x21 among them; a TSS limit that took in a zeroed bitmap would let every port through,
# up to 0xffff.
#
# Runs scripts/tests/exceptions.sh with only its I/O cases (one fault.elf boot each):
#   in      "in al, 0x80"         out     "out 0x80, al"
#   in21    "in al, 0x21"         out20   "out 0x20, al" (al = 0x0a, OCW3: read the IRR)
#   inffff  "in al, dx" with dx = 0xffff
# Each checks the #GP stub with error 0 and the saved ring-3 CS, the faulting instruction
# (e4 80 / e6 80 / e4 21 / e6 20 / ec, and dx = 0xffff), interrupt_handler's frame,
# process_terminate on the faulting process, "faulttest: survived" never shown, blank.elf from
# the same shell afterwards, task list, heap, and no panic.
#
# Env: as exceptions.sh

set -euo pipefail
cd "$(dirname "$0")/../.."
EXCEPTION_CASES="in out in21 out20 inffff" EXCEPTIONS_NAME=tss-io exec bash scripts/tests/exceptions.sh
