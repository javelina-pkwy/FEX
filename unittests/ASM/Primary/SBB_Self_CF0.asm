%ifdef CONFIG
{
  "RegData": {
    "R8":  "0x1111222233334400",
    "R9":  "0x44",
    "RBX": "0x4444555566660080",
    "R10": "0x44",
    "R11": "0x7777888899990000",
    "R12": "0x44",
    "R13": "0",
    "R14": "0x44",
    "R15": "0",
    "RAX": "0x44"
  }
}
%endif

; With CF clear, self-SBB produces zero with PF and ZF set.

mov rsp, 0xe000_1000

mov r8, 0x1111222233334480
clc
sbb r8b, r8b
pushfq
pop r9
and r9, 0x8d5

mov rbx, 0x4444555566664480
clc
sbb bh, bh
pushfq
pop r10
and r10, 0x8d5

mov r11, 0x7777888899994000
clc
sbb r11w, r11w
pushfq
pop r12
and r12, 0x8d5

mov r13, 0xAAAABBBB40000000
clc
sbb r13d, r13d
pushfq
pop r14
and r14, 0x8d5

mov r15, 0x4000000000000000
clc
sbb r15, r15
pushfq
pop rax
and rax, 0x8d5

hlt
