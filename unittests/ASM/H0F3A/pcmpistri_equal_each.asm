%ifdef CONFIG
{
  "RegData": {
      "XMM0": ["0x07000F060E060F00", "0x0000000007040404"],
      "XMM1": ["0x3939191919193939", "0x3939212119191919"],
      "XMM2": ["0xDDEE007964776F48", "0x5566778899AABBCC"],
      "XMM3": ["0x4546207964776F48", "0x212121736E616958"],
      "XMM4": ["0x0000000000000010", "0x0000000000000000"],
      "XMM5": ["0x0000000000003110", "0x0000000000000000"]
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

; Unsigned byte string check (lsb, positive polarity)
CompareAndStore 0, 0b00001000

; Unsigned byte string check (msb, positive polarity)
CompareAndStore 1, 0b01001000

; Unsigned byte string check (lsb, negative polarity)
CompareAndStore 2, 0b00011000

; Unsigned byte string check (msb, negative polarity)
CompareAndStore 3, 0b01011000

; Unsigned byte string check (lsb, negative masked)
CompareAndStore 4, 0b00111000

; Unsigned byte string check (msb, negative masked)
CompareAndStore 5, 0b01111000

; --- 16-bit unsigned word tests ---

movaps xmm2, [rel .data16]
movaps xmm3, [rel .data16 + 32]

; Unsigned word string check (lsb, positive polarity)
CompareAndStore 6, 0b00001001

; Unsigned word string check (msb, positive polarity)
CompareAndStore 7, 0b01001001

; Unsigned word string check (lsb, negative polarity)
CompareAndStore 8, 0b00011001

; Unsigned word string check (msb, negative polarity)
CompareAndStore 9, 0b01011001

; Unsigned word string check (lsb, negative masked)
CompareAndStore 10, 0b00111001

; Unsigned word string check (msb, negative masked)
CompareAndStore 11, 0b01111001

movaps xmm2, [rel .data_howdy]
movaps xmm3, [rel .data_howdy]
; Unsigned byte string check (lsb, positive masked)
CompareAndStore 12, 0b00101000

movaps xmm3, [rel .data_howdy_alt]
; Signed byte string check (lsb, positive polarity)
CompareAndStore 13, 0b00001010

movaps xmm2, [rel .data16_japanese]
movaps xmm3, [rel .data16_japanese]
; Identical word strings
CompareAndStore 14, 0b00001001

movaps xmm2, [rel .data_empty]
movaps xmm3, [rel .data_empty]
; Both strings empty (lsb)
CompareAndStore 15, 0b00001000

movaps xmm3, [rel .data_howdy]
; Empty string against a full string
CompareAndStore 16, 0b00001000

movaps xmm2, [rel .data_howdy_short]
; Short string against a full string (lsb)
CompareAndStore 17, 0b00001000

; Load all our stored indices and flags for result comparing
movaps xmm0, [rel .indices]
movaps xmm4, [rel .indices + 16]
movaps xmm1, [rel .flags]
movaps xmm5, [rel .flags + 16]

hlt

align 4096
.data:
dq 0x6550206F6C6C6548 ; "Hello Pe"
dq 0x00002121656C706F ; "ople!!\0\0"
dq 0xFFFFFFFFFFFFFFFF
dq 0xEEEEEEEEEEEEEEEE

dq 0x2759206F6C6C6548 ; "Hello Y'"
dq 0x00212121216C6C61 ; "all!!!!\0"
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

.data_howdy:
dq 0x4546207964776F48 ; "Howdy FE"
dq 0x212121736E616958 ; "Xians!!!"

.data_howdy_alt:
dq 0x4546207964776F48 ; "Howdy FE"
dq 0x3F2121736E616958 ; "Xians!!?"

.data16_japanese:
dq 0x306F8A9E672C65E5 ; "日本語は"
dq 0x00003044305796E3 ; "難しい\0"

.data_empty:
dq 0x8899AABBCCDDEE00 ; "\0" (followed by junk)
dq 0xF011223344556677

.data_howdy_short:
dq 0xDDEE007964776F48 ; "Howdy\0" (followed by junk)
dq 0x5566778899AABBCC

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
