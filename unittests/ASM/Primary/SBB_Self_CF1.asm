%ifdef CONFIG
{
  "RegData": {
    "R8":  "0x11112222333344FF",
    "R9":  "0x95",
    "RBX": "0x444455556666FF80",
    "R10": "0x95",
    "R11": "0x777788889999FFFF",
    "R12": "0x95",
    "R13": "0xFFFFFFFF",
    "R14": "0x95",
    "R15": "0xFFFFFFFFFFFFFFFF",
    "RAX": "0x95"
  }
}
%endif

; With CF set, self-SBB produces all-ones with CF, PF, AF, and SF set.

mov rsp, 0xe000_1000

mov r8, 0x1111222233334480
stc
sbb r8b, r8b
pushfq
pop r9
and r9, 0x8d5

mov rbx, 0x4444555566664480
stc
sbb bh, bh
pushfq
pop r10
and r10, 0x8d5

mov r11, 0x7777888899994000
stc
sbb r11w, r11w
pushfq
pop r12
and r12, 0x8d5

mov r13, 0xAAAABBBB40000000
stc
sbb r13d, r13d
pushfq
pop r14
and r14, 0x8d5

mov r15, 0x4000000000000000
stc
sbb r15, r15
pushfq
pop rax
and rax, 0x8d5

hlt
