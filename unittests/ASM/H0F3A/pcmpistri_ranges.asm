%ifdef CONFIG
{
  "RegData": {
      "XMM0": ["0x00060F000F000D01", "0x0010001010070007"],
      "XMM1": ["0x3111313131311111", "0x3918391818313131"],
      "XMM2": ["0xBBCCDDEE00417A61", "0x33445566778899AA"],
      "XMM3": ["0x007A797820434241", "0x778899AABBCCDDEE"],
      "XMM4": ["0x0000000000040100", "0x0000000000000000"],
      "XMM5": ["0x0000000000191931", "0x0000000000000000"]
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

; Range unsigned byte check (lsb, positive polarity)
CompareAndStore 0, 0b00000100

; Range unsigned byte check (msb, positive polarity)
CompareAndStore 1, 0b01000100

; Range unsigned byte check (lsb, negative polarity)
CompareAndStore 2, 0b00010100

; Range unsigned byte check (msb, negative polarity)
CompareAndStore 3, 0b01010100

; Range unsigned byte check (lsb, negative masked)
CompareAndStore 4, 0b00110100

; Range unsigned byte check (msb, negative masked)
CompareAndStore 5, 0b01110100

; --- 16-bit unsigned word tests ---
movaps xmm2, [rel .data16]
movaps xmm3, [rel .data16 + 32]

; Range unsigned word check (msb, positive polarity)
CompareAndStore 6, 0b01000101

; Range unsigned word check (lsb, negative polarity)
CompareAndStore 7, 0b00010101

; Range unsigned word check (msb, negative polarity)
CompareAndStore 8, 0b01010101

; Range unsigned word check (lsb, negative masked)
CompareAndStore 9, 0b00110101

; Range unsigned word check (msb, negative masked)
CompareAndStore 10, 0b01110101

; --- Edge case test (string begins with null character) ---
movaps xmm2, [rel .data_null]
movaps xmm3, [rel .data_null + 32]

; Range signed byte check (msb)
CompareAndStore 11, 0b01000110

; Range signed byte check (lsb)
CompareAndStore 12, 0b01000110

movaps xmm2, [rel .data_range_s8]
movaps xmm3, [rel .data_range_s8_str]
; Signed byte range check (lsb, positive polarity)
CompareAndStore 13, 0b00000110

; Same range as unsigned bytes (empty range)
CompareAndStore 14, 0b00000100

movaps xmm2, [rel .data16_range_s16]
movaps xmm3, [rel .data16_range_values]
; Signed word range check (lsb, positive polarity)
CompareAndStore 15, 0b00000111

movaps xmm2, [rel .data_range_all]
movaps xmm3, [rel .data_howdy]
; Range covering every non-null byte
CompareAndStore 16, 0b00000100

movaps xmm2, [rel .data_range_ff]
movaps xmm3, [rel .data_ff_str]
; Single character range (lsb)
CompareAndStore 17, 0b00000100

movaps xmm2, [rel .data_azA]
movaps xmm3, [rel .data_abc_xyz]
; Odd number of range characters
CompareAndStore 18, 0b00000100

; Load all our stored indices and flags for result comparing
movaps xmm0, [rel .indices]
movaps xmm4, [rel .indices + 16]
movaps xmm1, [rel .flags]
movaps xmm5, [rel .flags + 16]

hlt

align 4096
.data:
dq 0x998877005A417A61 ; "azAZ" (followed by junk)
dq 0x55AACCBBFF223344
dq 0xFFFFFFFFFFFFFFFF
dq 0xEEEEEEEEEEEEEEEE

dq 0x726548206D27493F ; "?I'm Her"
dq 0x21216E65704F2065 ; "e Open!!"
dq 0xFFFFFFFFFFFFFFFF
dq 0xEEEEEEEEEEEEEEEE

.data16:
dq 0x005A0041007A0061 ; "azAZ"
dq 0x55AACCBBFF220000
dq 0xAAAAAAAAAAAAAAAA
dq 0xBBBBBBBBBBBBBBBB

dq 0x006500200027003F ; "?' e"
dq 0x00210065004F0065 ; "eOen!"
dq 0x8888888888888888
dq 0x9999999999999999

.data_null:
dq 0x005A0041007A0061 ; "azAZ"
dq 0x55AACCBBFF220000
dq 0xAAAAAAAAAAAAAAAA
dq 0xBBBBBBBBBBBBBBBB

dq 0x0065002000270000 ; "\0' e"
dq 0x00210065004F0065 ; "eOen!"
dq 0x8888888888888888
dq 0x9999999999999999

.data_range_s8:
dq 0xAABBCCDDEE0002FE ; "\xfe\x02\0" (followed by junk)
dq 0x2233445566778899

.data_range_s8_str:
dq 0x004103FFFD807F01 ; "\x01\x7f\x80\xfd\xff\x03A\0"
dq 0x778899AABBCCDDEE

.data16_range_s16:
dq 0xEEDD00000002FFFE ; words 0xFFFE, 0x0002, 0 (followed by junk)
dq 0x66558877AA99CCBB

.data16_range_values:
dq 0xFFFD80007FFF0001 ; words 0x0001, 0x7FFF, 0x8000, 0xFFFD
dq 0x000000410003FFFF ; words 0xFFFF, 0x0003, 0x0041, 0

.data_range_all:
dq 0xAABBCCDDEE00FF01 ; "\x01\xff\0" (followed by junk)
dq 0x2233445566778899

.data_howdy:
dq 0x4546207964776F48 ; "Howdy FE"
dq 0x212121736E616958 ; "Xians!!!"

.data_range_ff:
dq 0xAABBCCDDEE00FFFF ; "\xff\xff\0" (followed by junk)
dq 0x2233445566778899

.data_ff_str:
dq 0xCCDDEE00FF62FF61 ; "a\xffb\xff\0" (followed by junk)
dq 0x445566778899AABB

.data_azA:
dq 0xBBCCDDEE00417A61 ; "azA\0" (followed by junk)
dq 0x33445566778899AA

.data_abc_xyz:
dq 0x007A797820434241 ; "ABC xyz\0"
dq 0x778899AABBCCDDEE

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
