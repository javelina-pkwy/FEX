%ifdef CONFIG
{
  "RegData": {
    "RAX": "0x41424344",
    "RBX": "0x5152537f",
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

; cmp byte [edi + 1], 0x61
stc
db 0x82, 0x7f, 0x01, 0x61

mov eax, [edi]

; cmp bl, 0x81
mov ebx, 0x5152537f
stc
db 0x82, 0xfb, 0x81

; CF, PF, ZF, SF, OF
pushfd
pop ecx
and ecx, 0x8c5

hlt
