%ifdef CONFIG
{
  "HostFeatures": ["AVX"],
  "RegData": {
      "RAX": ["0x100000002"],
      "RDX": ["16"],
      "XMM0": ["0x05050F000F000902", "0x0002100207000700", "0x0000000000100202", "0x0000000000000000"],
      "XMM1": ["0x1111313131311111", "0x3119181131313131", "0x0000000000001111", "0x0000000000000000"],
      "XMM2": ["0x99AABBCCDDEE0077", "0x1122334455667788", "0x0000000000000000", "0x0000000000000000"],
      "XMM3": ["0x4546207900776F48", "0x212121736E616958", "0x0000000000000000", "0x0000000000000000"]
  }
}
%endif

; Adjusts the result from LAHF and SETO so that we have a set of flags organized
; like [OF, SF, ZF, AF, PF, CF] for storing into the .flags region
; of memory.
;
; The first parameter is the byte offset to store the flag result
; at in the .flags region of memory.
;
%macro ArrangeAndStoreFLAGS 1
  lahf
  seto bl
  movzx bx, bl

  shr ax, 8
  shl bx, 5

  mov di, ax
  mov si, ax

  ; Mask and shift
  and di, 0b0000_0000_0000_0100 ; PF
  and si, 0b0000_0000_0001_0000 ; AF
  shr di, 1
  shr si, 2

  ; OR all of them together
  or bx, di
  or bx, si

  ; Reclaim DI for getting ZF/SF and shift into place
  mov di, ax
  and di, 0b0000_0000_1100_0000 ; ZF and SF
  shr di, 3

  ; Finally mask and OR all of the bits together
  and ax, 0b0000_0000_0000_0001 ; CF
  or bx, ax
  or bx, di

  ; Store result to .flags memory
  mov [rel .flags + %1], bl
%endmacro

; Performs the string comparison and moves the result from RCX to
; a region of memory in the .indices section specified by a byte
; offset.
;
; The first parameter is the byte offset to store the RCX result to.
; The second parameter is the control values to pass to vpcmpestri
;
%macro CompareAndStore 2
  vpcmpestri xmm2, xmm3, %2
  mov [rel .indices + %1], cl

  mov r15, rax
  ArrangeAndStoreFLAGS %1
  mov rax, r15
%endmacro

vmovaps ymm2, [rel .data]
vmovaps ymm3, [rel .data + 32]

; Unsigned byte string check (lsb, positive polarity)
mov rax, 2
mov rdx, 16
CompareAndStore 0, 0b00001100

; Unsigned byte string check (msb, positive polarity)
CompareAndStore 1, 0b01001100

; Unsigned byte string check (lsb, negative polarity)
CompareAndStore 2, 0b00011100

; Unsigned byte string check (msb, negative polarity)
CompareAndStore 3, 0b01011100

; Unsigned byte string check (lsb, negative masked)
CompareAndStore 4, 0b00111100

; Unsigned byte string check (msb, negative masked)
CompareAndStore 5, 0b01111100

; --- 16-bit unsigned word tests ---
; Intentionally don't reset RDX to 8 here to test upper bounds clamping.
vmovaps ymm2, [rel .data16]
vmovaps ymm3, [rel .data16 + 32]

CompareAndStore 6, 0b00001101

; Unsigned word string check (msb, positive polarity)
CompareAndStore 7, 0b01001101

; Unsigned word string check (lsb, negative polarity)
CompareAndStore 8, 0b00011101

; Unsigned word string check (msb, negative polarity)
CompareAndStore 9, 0b01011101

; Unsigned word string check (lsb, negative masked)
CompareAndStore 10, 0b00111101

; Unsigned word string check (msb, negative masked)
CompareAndStore 11, 0b01111101

vmovaps xmm2, [rel .data_w]
vmovaps xmm3, [rel .data_howdy_null]
mov rax, 2
mov rdx, 16
; Null character inside both lengths (lsb)
CompareAndStore 12, 0b00001100

mov rdx, 3
; Needle running past the string length
CompareAndStore 13, 0b00001100

mov rdx, 4
; Needle ending exactly at the string length
CompareAndStore 14, 0b00001100

mov rax, 0
mov rdx, 16
; Zero length needle
CompareAndStore 15, 0b00001100

mov rax, -2
; Negative length
CompareAndStore 16, 0b00001100

mov rax, 0x100000002
; Upper 32 bits of RAX ignored without REX.W
CompareAndStore 17, 0b00001100

; REX.W uses all 64 bits of RAX
db 0xC4, 0xE3, 0xF9, 0x61, 0xD3, 0b00001100 ; VEX.W1 vpcmpestri xmm2, xmm3
mov [rel .indices + 18], cl
mov r15, rax
ArrangeAndStoreFLAGS 18
mov rax, r15

; Load all our stored indices and flags for result comparing
vmovaps ymm0, [rel .indices]
vmovaps ymm1, [rel .flags]

hlt

align 4096
.data:
dq 0x6550206F6FFF6C6C ; "ll" with junk following it
dq 0x21212121656C706F
dq 0xFFFFFFFFFFFFFFFF
dq 0xEEEEEEEEEEEEEEEE

dq 0x2759206F6C6C6548 ; "Hello Y'"
dq 0x21212121216C6C61 ; "all!!!!!"
dq 0xDDDDDDDDDDDDDDDD
dq 0xCCCCCCCCCCCCCCCC

.data16:
dq 0x306F8A9E30443057 ; "しい" followed by junk
dq 0x000030443057697D
dq 0xAAAAAAAAAAAAAAAA
dq 0xBBBBBBBBBBBBBBBB

dq 0x306F8A9E672C65E5 ; "日本語は"
dq 0x00003044305796E3 ; "難しい\0" (Japanese is hard)
dq 0x8888888888888888
dq 0x9999999999999999

.data_w:
dq 0x99AABBCCDDEE0077 ; "w\0" (followed by junk)
dq 0x1122334455667788

.data_howdy_null:
dq 0x4546207900776F48 ; "How\0y FE"
dq 0x212121736E616958 ; "Xians!!!"

.indices:
dq 0x0000000000000000
dq 0x0000000000000000
dq 0x0000000000000000
dq 0x0000000000000000

.flags:
dq 0x0000000000000000
dq 0x0000000000000000
dq 0x0000000000000000
dq 0x0000000000000000
