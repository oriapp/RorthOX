ORG 0x7c00
BITS 16

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start


jmp short start
nop

; times 33 db 0 ; Here will be in the future the filesystem
; FAT16 header
OEMIdentifier           db 'POTONGOS'
BytesPerSector          dw 0x200
SectorsPerCluster       db 0x80
ReservedSectors         dw 200
FATCopies               db 0x02
RootDirEntries          dw 0x40
NumSectors              dw 0x00
MediaType               db 0xF8
SectorsPerFat           dw 0x100
SectorsPerTrack         dw 0x20
NumberOfHeads           dw 0x40
HiddenSectors           dd 0x00
SectorsBig              dd 0x773594

; Extended BPB (Dos 4.0)
DriveNumber             db 0x80
WinNTBit                db 0x00
Signature               db 0x29
VolumeID                dd 0xD105
VolumeIDString          db 'POTONGOS BO'
SystemIDString          db 'FAT16   '



start:
    jmp 0: step2

step2:

    cli ;   Clear Interrupts
    mov ax, 0x00
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti ;   Enable Interrupts
    

.load_protected:
    cli
    lgdt[gdt_descriptor]
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp CODE_SEG: load32    ;   jmp 0x08: laod32


;   GDT
gdt_start:
gdt_null:
    dd 0x0
    dd 0x0
    ; 64 bits of 0

; offset 0x8
gdt_code:       ;   CS SHOULD POINT TO THIS
    dw 0xffff   ;   Segment limit 0-15 bits
    dw 0        ;   Base first 0-15 bits
    db 0        ;   Base 16-23 bits
    db 0x9a     ;   Access byte
    db 11001111b    ; High 4 bit flags and the low 4 bit flags
    db 0        ;   Base 24 - 31 bits


;   offset 0x10
gdt_data:       ;   DS, SS, ES, FS, GS
    dw 0xffff   ;   Segment limit 0-15 bits
    dw 0        ;   Base first 0-15 bits
    db 0        ;   Base 16-23 bits
    db 0x92     ;   Access byte
    db 11001111b    ; High 4 bit flags and the low 4 bit flags
    db 0        ;   Base 24 - 31 bits

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1  ;   Size of the descriptor
    dd gdt_start

[BITS 32]
load32:                 ;   0 is the boot sector
    mov eax, 1          ;   Starting sector
    mov ecx, 100        ;   100 sectors of null
    mov edi, 0x0100000  ;   1MG (related to rep insw
    call ata_lba_read
    ; call ata_lba_write
    jmp CODE_SEG: 0x0100000


ata_lba_read:
    ; Written it in lexture number 17.
    mov ebx, eax    ;   Backup the LBA
    ; Send the highest 8 bits of the lba hard disk controller
    shr eax, 24     ; Short eax register 24 bits to the right (32 - 24 = 8)
    ;   EAX after the line above will contain the 8 highest bits of the LBA
    or eax, 0xE0    ; Select the master drive (there is slave and master)
    mov dx, 0x1F6
    out dx, al      ; AL is bits containing highest 8 bits of the LBA
    ; Finished sending the highest 8 bits of the LBA

    ; Send the total sectors to read
    mov eax, ecx
    mov dx, 0x1F2
    out dx, al
    ; Finished sending the total sectors to read

    ; Send more bits of the LBA
    mov eax, ebx    ; Restore the backup LBA
    mov dx, 0x1F3
    out dx, al
    ; Finished sending more bits of the LBA

    ; Send more bits of the LBA
    mov dx, 0x1F4
    mov eax, ebx    ; Restore the backup LBA 
    shr eax, 8
    out dx, al
    ; Finished sending more bits of the LBA

    ; Send upper 16 bits of the LBA
    mov dx, 0x1F5
    mov eax, ebx    ; Restore the backup LBA
    shr eax, 16
    out dx, al
    ; Finished sending upper 16 bits of the LBA

    mov dx, 0x1F7
    mov al, 0x20
    out dx, al

    ; Read all sectors into memory
.next_sector:
    push ecx

; Checking if we need to read
.try_again:
    mov dx, 0x1F7
    in al, dx
    test al, 8
    jz .try_again

; We need to read 256 words at a time
    mov ecx, 256    ; 256 words is 512 bytes (: i.e one sector
    ; ^ Do the insw instruction 256 times
    mov dx, 0x1F0
    rep insw    ; Input word from I/O port specified in DX into memory location specified in ES:(E)DI
    ; Reading from port 0x1F0 and storing it into "0x0100000"
    pop ecx
    loop .next_sector
    ; End of reading sectors into memory
    ret

ata_lba_write:
    ; Backup the LBA
    mov ebx, eax

    ; Send the highest 8 bits of the LBA to hard disk controller
    shr eax, 24
    or eax, 0xE0    ; Select the master drive (there is slave and master)
    mov dx, 0x1F6
    out dx, al

    ; Send the total sectors to write
    mov eax, ecx
    mov dx, 0x1F2
    out dx, al

    ; Send more bits of the LBA
    mov eax, ebx
    mov dx, 0x1F3
    out dx, al

    ; Send more bits of the LBA
    mov dx, 0x1F4
    mov eax, ebx
    shr eax, 8
    out dx, al

    ; Send upper 16 bits of the LBA
    mov dx, 0x1F5
    mov eax, ebx
    shr eax, 16
    out dx, al

    ; Send the command to write sectors
    mov dx, 0x1F7
    mov al, 0x30    ; Command for writing sectors
    out dx, al

    ; Write all sectors from memory
.next_sector:
    push ecx

    ; Check if we need to write
.try_again:
    mov dx, 0x1F7
    in al, dx
    test al, 8
    jz .try_again

    ; We need to write 256 words at a time
    mov ecx, 256
    mov dx, 0x1F0
    rep outsw    ; Output word from memory location specified in DS:(E)SI to I/O port specified in DX
    ; Writing to port 0x1F0 from "0x0100000" (memory)
    pop ecx
    loop .next_sector

    ; End of writing sectors to disk
    ret

times 510-($ - $$) db 0
dw 0xAA55


; Resource: Filesystem
; https://wiki.osdev.org/FAT
; FAT16 Boot Sector: 
; http://www.maverick-os.dk/FileSystemFormats/FAT16_FileSystem.html

;   gdb
;   target remote | qemu-system-x86_64 -hda boot.bin -S -gdb stdio
; c - to continue
; Hold "control + c" and we can see where is the program curentlly executing.
; layout asm
; info registers