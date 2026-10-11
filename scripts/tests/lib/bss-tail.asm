; Link fixture for scripts/tests/kernel-bss.sh, never part of bin/os.bin: an odd-sized .bss.*
; input section that the test links after the kernel objects, so .bss ends on an odd address.
section .bss.test_tail nobits alloc write noexec align=1
global bss_test_tail
bss_test_tail:
    resb 4099
