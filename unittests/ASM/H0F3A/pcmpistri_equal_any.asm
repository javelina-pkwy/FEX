%ifdef CONFIG
{
  "RegData": {
      "XMM0": ["0x04060F000F000D07", "0x0202011001040407"],
      "XMM1": ["0x1939313131311111", "0x1919191011191919"],
      "XMM2": ["0xAABBCCDDEE0080FF", "0x2233445566778899"],
      "XMM3": ["0xEE00646380FF6261", "0x66778899AABBCCDD"]
  },
  "HostFeatures": ["SSE4.2"]
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
; The second parameter is the control values to pass to pcmpistri
;
%macro CompareAndStore 2
  pcmpistri xmm2, xmm3, %2
  mov [rel .indices + %1], cl
  ArrangeAndStoreFLAGS %1
%endmacro

movaps xmm2, [rel .data]
movaps xmm3, [rel .data + 32]

; Unsigned byte character check (lsb, positive polarity)
CompareAndStore 0, 0b00000000

; Unsigned byte character check (msb, positive polarity)
CompareAndStore 1, 0b01000000

; Unsigned byte character check (lsb, negative polarity)
CompareAndStore 2, 0b00010000

; Unsigned byte character check (msb, negative polarity)
CompareAndStore 3, 0b01010000

; Unsigned byte character check (lsb, negative masked)
CompareAndStore 4, 0b00110000

; Unsigned byte character check (msb, negative masked)
CompareAndStore 5, 0b01110000

; --- 16-bit unsigned word tests ---
movaps xmm2, [rel .data16]
movaps xmm3, [rel .data16 + 32]

; Unsigned word character check (msb, positive polarity)
CompareAndStore 6, 0b01000001

; Unsigned word character check (lsb, negative polarity)
CompareAndStore 7, 0b00010001

; Unsigned word character check (msb, negative polarity)
CompareAndStore 8, 0b01010001

; Unsigned word character check (lsb, negative masked)
CompareAndStore 9, 0b00110001

; Unsigned word character check (msb, negative masked)
CompareAndStore 10, 0b01110001

movaps xmm2, [rel .data_vowels]
movaps xmm3, [rel .data_howdy]
; Unsigned byte character check (lsb, positive masked)
CompareAndStore 11, 0b00100000

movaps xmm2, [rel .data_empty]
; Empty character set
CompareAndStore 12, 0b00000000

movaps xmm2, [rel .data_vowels]
movaps xmm3, [rel .data_hi]
; Short string with set members past the null terminator (lsb)
CompareAndStore 13, 0b00000000

movaps xmm2, [rel .data_highbits]
movaps xmm3, [rel .data_highbits_str]
; Characters above 0x7F
CompareAndStore 14, 0b00000000

; Characters above 0x7F, signed
CompareAndStore 15, 0b00000010

; Load all our stored indices and flags for result comparing
movaps xmm0, [rel .indices]
movaps xmm1, [rel .flags]

hlt

align 4096
.data:
dq 0x6463626144434241 ; "ABCDabcd"
dq 0x6C6B6A694C4B4A00 ; "\0JKLijkl"
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

.data_vowels:
dq 0xDDEE00756F696561 ; "aeiou\0" (followed by junk)
dq 0x5566778899AABBCC

.data_howdy:
dq 0x4546207964776F48 ; "Howdy FE"
dq 0x212121736E616958 ; "Xians!!!"

.data_empty:
dq 0x8899AABBCCDDEE00 ; "\0" (followed by junk)
dq 0xF011223344556677

.data_hi:
dq 0x756F696561006948 ; "Hi\0aeiou"
dq 0x778899AABBCCDDEE

.data_highbits:
dq 0xAABBCCDDEE0080FF ; "\xff\x80\0" (followed by junk)
dq 0x2233445566778899

.data_highbits_str:
dq 0xEE00646380FF6261 ; "ab\xff\x80cd\0" (followed by junk)
dq 0x66778899AABBCCDD

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
