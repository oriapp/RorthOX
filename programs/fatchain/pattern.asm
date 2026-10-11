; Pattern data for fatchain.elf (programs/fatchain/fatchain.c): FATCHAIN_WORDS words in .rodata,
; so they are part of the ELF file and reach several clusters past the start of the file.
; Word i is (i * 2654435761 + 0x9E3779B9) mod 2^32.
[BITS 32]

%define FATCHAIN_WORDS 51200

global fatchain_pattern
global fatchain_pattern_end

section .rodata
align 4
fatchain_pattern:
%assign i 0
%rep FATCHAIN_WORDS
    dd (i * 2654435761 + 0x9E3779B9) & 0xFFFFFFFF
%assign i i+1
%endrep
fatchain_pattern_end:
