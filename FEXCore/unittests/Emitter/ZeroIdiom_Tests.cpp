// SPDX-License-Identifier: MIT
#include "TestDisassembler.h"

#include <catch2/catch_test_macros.hpp>

using namespace ARMEmitter;

TEST_CASE_METHOD(TestDisassembler, "Emitter: ALU: Zero idioms") {
  TEST_SINGLE(eor(Size::i32Bit, Reg::r29, Reg::r28, Reg::r28), "eor w29, wzr, wzr");
  TEST_SINGLE(eor(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28), "eor x29, xzr, xzr");

  TEST_SINGLE(sub(Size::i32Bit, Reg::r29, Reg::r28, Reg::r28), "neg w29, wzr");
  TEST_SINGLE(sub(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28), "neg x29, xzr");

  TEST_SINGLE(subs(Size::i32Bit, Reg::r29, Reg::r28, Reg::r28), "negs w29, wzr");
  TEST_SINGLE(subs(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28), "negs x29, xzr");

  TEST_SINGLE(cmp(Size::i32Bit, Reg::r28, Reg::r28), "cmp wzr, wzr");
  TEST_SINGLE(cmp(Size::i64Bit, Reg::r28, Reg::r28), "cmp xzr, xzr");

  TEST_SINGLE(sbc(Size::i32Bit, Reg::r29, Reg::r28, Reg::r28), "ngc w29, wzr");
  TEST_SINGLE(sbc(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28), "ngc x29, xzr");

  TEST_SINGLE(sbcs(Size::i32Bit, Reg::r29, Reg::r28, Reg::r28), "ngcs w29, wzr");
  TEST_SINGLE(sbcs(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28), "ngcs x29, xzr");

  // A zero shift amount is still a zero idiom regardless of shift type.
  TEST_SINGLE(eor(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28, ShiftType::LSR, 0), "eor x29, xzr, xzr");
  TEST_SINGLE(sub(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28, ShiftType::ASR, 0), "neg x29, xzr");
  TEST_SINGLE(subs(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28, ShiftType::LSR, 0), "negs x29, xzr");
  TEST_SINGLE(cmp(Size::i64Bit, Reg::r28, Reg::r28, ShiftType::ASR, 0), "cmp xzr, xzr");

  // Shifted equal-register operands are not zero idioms.
  TEST_SINGLE(eor(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28, ShiftType::LSL, 1), "eor x29, x28, x28, lsl #1");
  TEST_SINGLE(sub(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28, ShiftType::LSL, 1), "sub x29, x28, x28, lsl #1");
  TEST_SINGLE(subs(Size::i64Bit, Reg::r29, Reg::r28, Reg::r28, ShiftType::LSL, 1), "subs x29, x28, x28, lsl #1");
  TEST_SINGLE(cmp(Size::i64Bit, Reg::r28, Reg::r28, ShiftType::LSL, 1), "cmp x28, x28, lsl #1");
}

TEST_CASE_METHOD(TestDisassembler, "Emitter: ALU: Add/subtract zero canonicalizes to MOV") {
  TEST_SINGLE(add(Size::i32Bit, Reg::r29, Reg::r28, 0), "mov w29, w28");
  TEST_SINGLE(add(Size::i64Bit, Reg::r29, Reg::r28, 0), "mov x29, x28");
  TEST_SINGLE(sub(Size::i32Bit, Reg::r29, Reg::r28, 0), "mov w29, w28");
  TEST_SINGLE(sub(Size::i64Bit, Reg::r29, Reg::r28, 0), "mov x29, x28");

  // A shifted zero is still zero.
  TEST_SINGLE(add(Size::i64Bit, Reg::r29, Reg::r28, 0, true), "mov x29, x28");
  TEST_SINGLE(sub(Size::i64Bit, Reg::r29, Reg::r28, 0, true), "mov x29, x28");

  // Register 31 means SP for these encodings, not ZR, so the ADD/SUB form must
  // be preserved when either operand is SP. ADD by zero is itself the
  // architectural MOV alias for SP, so it still disassembles as a move.
  TEST_SINGLE(add(Size::i64Bit, Reg::rsp, Reg::r28, 0), "mov sp, x28");
  TEST_SINGLE(add(Size::i64Bit, Reg::r29, Reg::rsp, 0), "mov x29, sp");
  TEST_SINGLE(sub(Size::i64Bit, Reg::rsp, Reg::r28, 0), "sub sp, x28, #0x0 (0)");
  TEST_SINGLE(sub(Size::i64Bit, Reg::r29, Reg::rsp, 0), "sub x29, sp, #0x0 (0)");

  // Flag-setting variants must never become a MOV.
  TEST_SINGLE(adds(Size::i64Bit, Reg::r29, Reg::r28, 0), "adds x29, x28, #0x0 (0)");
  TEST_SINGLE(subs(Size::i64Bit, Reg::r29, Reg::r28, 0), "subs x29, x28, #0x0 (0)");
}
