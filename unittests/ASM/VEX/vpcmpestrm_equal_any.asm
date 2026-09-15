%ifdef CONFIG
{
  "HostFeatures": ["AVX"],
  "RegData": {
      "RAX":   ["2"],
      "RDX":   ["5"],
      "XMM1":  ["0x0121313131311111", "0x0000000019010101", "0x0000000000000000", "0x0000000000000000"],
      "XMM2":  ["0x99AABBCCDDEE6100", "0x1122334455667788", "0x0000000000000000", "0x0000000000000000"],
      "XMM3":  ["0xCCDDEE0062006178", "0x445566778899AABB", "0x0000000000000000", "0x0000000000000000"],
      "XMM4":  ["0x00000000000060A0", "0x0000000000000000", "0x0000000000000000", "0x0000000000000000"],
      "XMM5":  ["0xFF00FF0000000000", "0x00FFFF0000000000", "0x0000000000000000", "0x0000000000000000"],
      "XMM6":  ["0x0000000000009F5F", "0x0000000000000000", "0x0000000000000000", "0x0000000000000000"],
      "XMM7":  ["0x00FF00FFFFFFFFFF", "0xFF0000FFFFFFFFFF", "0x0000000000000000", "0x0000000000000000"],
      "XMM8":  ["0x0000000000009F5F", "0x0000000000000000", "0x0000000000000000", "0x0000000000000000"],
      "XMM9":  ["0x00FF00FFFFFFFFFF", "0xFF0000FFFFFFFFFF", "0x0000000000000000", "0x0000000000000000"],
      "XMM10": ["0xFFFFFFFFFFFFFFFF", "0xFFFFFFFFFFFF0000", "0x0000000000000000", "0x0000000000000000"],
      "XMM11": ["0x0000000000000010", "0x0000000000000000", "0x0000000000000000", "0x0000000000000000"],
      "XMM12": ["0x0000000000000000", "0x000000000000FFFF", "0x0000000000000000", "0x0000000000000000"],
      "XMM13": ["0x0000000000000010", "0x0000000000000000", "0x0000000000000000", "0x0000000000000000"],
      "XMM14": ["0x0000000000000000", "0x000000000000FFFF", "0x0000000000000000", "0x0000000000000000"],
      "XMM15": ["0x0000000000000016", "0x0000000000000000", "0x0000000000000000", "0x0000000000000000"]
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

; Performs the string comparison and moves the result from XMM0 to
; a specified vector in the third argument
;
; The first parameter is the location in memory result flags into.
; The second parameter is the control values to pass to pcmpistrm
; The third parameter is the XMM number to store the result in XMM0 to.
;
%macro CompareAndStore 3
  vpcmpestrm xmm2, xmm3, %2
  vmovaps xmm%3, xmm0

  mov r15, rax
  ArrangeAndStoreFLAGS %1
  mov rax, r15
%endmacro

vmovaps ymm2, [rel .data]
vmovaps ymm3, [rel .data + 32]

; Unsigned byte character check (bits, positive polarity)
mov rax, 15 ; Exclude 'l'
mov rdx, 16
CompareAndStore 0, 0b00000000, 4

; Unsigned byte character check (mask, positive polarity)
CompareAndStore 1, 0b01000000, 5

; Unsigned byte character check (bits, negative polarity)
CompareAndStore 2, 0b00010000, 6

; Unsigned byte character check (mask, negative polarity)
CompareAndStore 3, 0b01010000, 7

; Unsigned byte character check (bits, negative masked)
CompareAndStore 4, 0b00110000, 8

; Unsigned byte character check (mask, negative masked)
CompareAndStore 5, 0b01110000, 9

; --- 16-bit unsigned word tests ---
vmovaps ymm2, [rel .data16]
vmovaps ymm3, [rel .data16 + 32]

; Unsigned word character check (mask, positive polarity)
CompareAndStore 6, 0b01000001, 10

; Unsigned word character check (bits, negative polarity)
CompareAndStore 7, 0b00010001, 11

; Unsigned word character check (mask, negative polarity)
CompareAndStore 8, 0b01010001, 12

; Unsigned word character check (bits, negative masked)
CompareAndStore 9, 0b00110001, 13

; Unsigned word character check (mask, negative masked)
CompareAndStore 10, 0b01110001, 14

; Load all our stored flags for result comparing

vmovaps xmm2, [rel .data_null_a]
vmovaps xmm3, [rel .data_xa]
mov rax, 2
mov rdx, 5
; Null character in the set (lsb)
CompareAndStore 11, 0b00000000, 15

; Load all our stored indices and flags for result comparing
vmovaps ymm1, [rel .flags]

hlt

align 4096
.data:
dq 0x6463626144434241 ; "ABCDabcd"
dq 0x6C6B6A694C4B4A49 ; "IJKLijkl"
dq 0xFFFFFFFFFFFFFFFF
dq 0xEEEEEEEEEEEEEEEE

dq 0x4120492065726548 ; "Here I A"
dq 0x6C4C612759202C6D ; "m, Y'aLl"
dq 0xDDDDDDDDDDDDDDDD
dq 0xCCCCCCCCCCCCCCCC

.data16:
dq 0x306F8A9E672C65E5 ; "日本語は"
dq 0x000030443057697D ; "楽しい\0" (Japanese is fun)
dq 0xAAAAAAAAAAAAAAAA
dq 0xBBBBBBBBBBBBBBBB

dq 0x306F8A9E672C65E5 ; "日本語は"
dq 0x00003044305796E3 ; "難しい\0" (Japanese is hard)
dq 0x8888888888888888
dq 0x9999999999999999

.data_null_a:
dq 0x99AABBCCDDEE6100 ; "\0a" (followed by junk)
dq 0x1122334455667788

.data_xa:
dq 0xCCDDEE0062006178 ; "xa\0b\0" (followed by junk)
dq 0x445566778899AABB

align 32
.flags:
dq 0x0000000000000000
dq 0x0000000000000000
dq 0x0000000000000000
dq 0x0000000000000000
