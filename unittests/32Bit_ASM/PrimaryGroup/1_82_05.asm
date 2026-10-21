%ifdef CONFIG
{
  "RegData": {
    "RAX": "0x41e1e244",
    "RBX": "0x515253fe",
    "RCX": "0x881"
  },
  "Mode": "32BIT"
}
%endif

; 0x82 is the 32-bit only alias of the 0x80 group 1 encoding.
; NASM never emits it, so the instructions are encoded by hand.
mov edi, 0xe0000000
lea esp, [edi + 64]
mov dword [edi], 0x41424344

; sub byte [edi + 1], 0x61
stc
db 0x82, 0x6f, 0x01, 0x61

; lock sub byte [edi + 2], 0x61
clc
db 0xf0, 0x82, 0x6f, 0x02, 0x61

mov eax, [edi]

; sub bl, 0x81
mov ebx, 0x5152537f
stc
db 0x82, 0xeb, 0x81

; CF, PF, ZF, SF, OF
pushfd
pop ecx
and ecx, 0x8c5

hlt
