// SPDX-License-Identifier: MIT
#pragma once
#include "Common/SoftFloat.h"

#include "Interface/Core/Interpreter/Fallbacks/FallbackOpHandler.h"
#include "Interface/IR/IR.h"

#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/SHMStats.h>

namespace FEXCore::CPU {
FEXCORE_PRESERVE_ALL_ATTR static softfloat_state SoftFloatStateFromFCW(uint16_t FCW, bool Force80BitPrecision = false) {
  softfloat_state State {};
  State.detectTininess = softfloat_tininess_afterRounding;
  State.exceptionFlags = 0;
  State.roundingPrecision = 80;

  if (!Force80BitPrecision) {
    auto PC = (FCW >> 8) & 3;
    switch (PC) {
    case 0: State.roundingPrecision = 32; break;
    case 2: State.roundingPrecision = 64; break;
    case 3: State.roundingPrecision = 80; break;
    case 1: LOGMAN_MSG_A_FMT("Invalid x87 precision mode, {}", PC);
    }
  }

  auto RC = (FCW >> 10) & 3;
  switch (RC) {
  case 0: State.roundingMode = softfloat_round_near_even; break;
  case 1: State.roundingMode = softfloat_round_min; break;
  case 2: State.roundingMode = softfloat_round_max; break;
  case 3: State.roundingMode = softfloat_round_minMag; break;
  }

  return State;
}

// The x87 exception flags are sticky: OR in whatever softfloat raised.
FEXCORE_PRESERVE_ALL_ATTR static void HandleX87Exception(const softfloat_state& State, FEXCore::Core::CpuStateFrame* Frame) {
  const auto Raise = [&](uint8_t Flag, unsigned Loc) {
    if (State.exceptionFlags & Flag) {
      Frame->State.flags[Loc] = 1;
    }
  };
  Raise(softfloat_flag_invalid, FEXCore::X86State::X87FLAG_IE_LOC);
  Raise(softfloat_flag_infinite, FEXCore::X86State::X87FLAG_ZE_LOC);
  Raise(softfloat_flag_overflow, FEXCore::X86State::X87FLAG_OE_LOC);
  Raise(softfloat_flag_underflow, FEXCore::X86State::X87FLAG_UE_LOC);
  Raise(softfloat_flag_inexact, FEXCore::X86State::X87FLAG_PE_LOC);
}

// Wrapper for SoftFloat state to handle X87 exceptions
class ScopedSoftFloatState {
public:
  FEXCORE_PRESERVE_ALL_ATTR ScopedSoftFloatState(uint16_t FCW, FEXCore::Core::CpuStateFrame* Frame, bool Force80BitPrecision = false)
    : State(SoftFloatStateFromFCW(FCW, Force80BitPrecision))
    , Frame(Frame) {}

  FEXCORE_PRESERVE_ALL_ATTR ~ScopedSoftFloatState() {
    HandleX87Exception(State, Frame);
  }

  // Disable copy and move to ensure RAII semantics
  ScopedSoftFloatState(const ScopedSoftFloatState&) = delete;
  ScopedSoftFloatState& operator=(const ScopedSoftFloatState&) = delete;
  ScopedSoftFloatState(ScopedSoftFloatState&&) = delete;
  ScopedSoftFloatState& operator=(ScopedSoftFloatState&&) = delete;

  softfloat_state State;
  FEXCore::Core::CpuStateFrame* Frame;
};

// The x87 raises #IA for pseudo-NaN, pseudo-infinity and unnormal operands instead of computing
// with them; the masked result is the QNaN indefinite. Otherwise a denormal or pseudo-denormal
// operand raises #D (invalid operands take precedence, as on hardware).
FEXCORE_PRESERVE_ALL_ATTR static bool InvalidOperand(ScopedSoftFloatState& State, const X80SoftFloat& Src1, const X80SoftFloat* Src2 = nullptr) {
  const auto Denormal = [](const X80SoftFloat& Src) {
    return Src.Top.Exponent == 0 && Src.Significand != 0;
  };
  if (X80SoftFloat::IsUnsupported(Src1) || (Src2 && X80SoftFloat::IsUnsupported(*Src2))) {
    State.State.exceptionFlags |= softfloat_flag_invalid;
    return true;
  }
  if (Denormal(Src1) || (Src2 && Denormal(*Src2))) {
    State.Frame->State.flags[FEXCore::X86State::X87FLAG_DE_LOC] = 1;
  }
  return false;
}

FEXCORE_PRESERVE_ALL_ATTR static bool InvalidOperand(ScopedSoftFloatState& State, const X80SoftFloat& Src1, const X80SoftFloat& Src2) {
  return InvalidOperand(State, Src1, &Src2);
}

// The trigonometric instructions report an argument of 2^63 or more through C2 and leave ST(0)
// unchanged. The dispatcher clears C2 before the operation so this survives it.
FEXCORE_PRESERVE_ALL_ATTR static bool OutOfTrigRange(const X80SoftFloat& Src, FEXCore::Core::CpuStateFrame* Frame) {
  if (!X80SoftFloat::IsOutOfTrigRange(Src)) {
    return false;
  }
  Frame->State.flags[FEXCore::X86State::X87FLAG_C2_LOC] = 1;
  return true;
}

// FPREM/FPREM1 report a partial reduction through C2 and otherwise the low three quotient bits
// through C0 (bit 2), C3 (bit 1) and C1 (bit 0).
FEXCORE_PRESERVE_ALL_ATTR static void SetRemainderFlags(FEXCore::Core::CpuStateFrame* Frame, bool Partial, uint8_t Quotient) {
  Frame->State.flags[FEXCore::X86State::X87FLAG_C2_LOC] = Partial;
  Frame->State.flags[FEXCore::X86State::X87FLAG_C0_LOC] = (Quotient >> 2) & 1;
  Frame->State.flags[FEXCore::X86State::X87FLAG_C3_LOC] = (Quotient >> 1) & 1;
  Frame->State.flags[FEXCore::X86State::X87FLAG_C1_LOC] = Quotient & 1;
}

template<>
struct OpHandlers<IR::OP_F80CVTTO> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle4(uint16_t FCW, float src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    // Loading a denormal single or double raises #D.
    const auto Bits = std::bit_cast<uint32_t>(src);
    if ((Bits & 0x7F800000U) == 0 && (Bits & 0x007FFFFFU) != 0) {
      Frame->State.flags[FEXCore::X86State::X87FLAG_DE_LOC] = 1;
    }
    return X80SoftFloat(&State.State, src);
  }

  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle8(uint16_t FCW, double src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    const auto Bits = std::bit_cast<uint64_t>(src);
    if ((Bits & 0x7FF0000000000000ULL) == 0 && (Bits & 0x000FFFFFFFFFFFFFULL) != 0) {
      Frame->State.flags[FEXCore::X86State::X87FLAG_DE_LOC] = 1;
    }
    return X80SoftFloat(&State.State, src);
  }
};

template<>
struct OpHandlers<IR::OP_F80CMP> {
  FEXCORE_PRESERVE_ALL_ATTR static uint64_t handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};

    bool eq, lt, nan;
    uint64_t ResultFlags = 0;

    if (InvalidOperand(State, Src1, Src2)) {
      return 1 << IR::FCMP_FLAG_UNORDERED;
    }

    X80SoftFloat::FCMP(&State.State, Src1, Src2, &eq, &lt, &nan);
    if (lt) {
      ResultFlags |= (1 << IR::FCMP_FLAG_LT);
    }
    if (nan) {
      ResultFlags |= (1 << IR::FCMP_FLAG_UNORDERED);
    }
    if (eq) {
      ResultFlags |= (1 << IR::FCMP_FLAG_EQ);
    }
    return ResultFlags;
  }
};

template<>
struct OpHandlers<IR::OP_F80CVT> {
  FEXCORE_PRESERVE_ALL_ATTR static float handle4(uint16_t FCW, VectorRegType src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    return X80SoftFloat(src).ToF32(&State.State);
  }

  FEXCORE_PRESERVE_ALL_ATTR static double handle8(uint16_t FCW, VectorRegType src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    return X80SoftFloat(src).ToF64(&State.State);
  }
};

template<>
struct OpHandlers<IR::OP_F80CVTINT> {
  FEXCORE_PRESERVE_ALL_ATTR static int16_t handle2(uint16_t FCW, VectorRegType src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    return X80SoftFloat(src).ToI16(&State.State);
  }

  FEXCORE_PRESERVE_ALL_ATTR static int32_t handle4(uint16_t FCW, VectorRegType src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    return X80SoftFloat(src).ToI32(&State.State);
  }

  FEXCORE_PRESERVE_ALL_ATTR static int64_t handle8(uint16_t FCW, VectorRegType src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    return X80SoftFloat(src).ToI64(&State.State);
  }

  FEXCORE_PRESERVE_ALL_ATTR static int16_t handle2t(uint16_t FCW, VectorRegType src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    auto rv = extF80_to_i32(&State.State, X80SoftFloat(src), softfloat_round_minMag, false);

    if (rv > INT16_MAX || rv < INT16_MIN) {
      ///< Indefinite value for 16-bit conversions.
      return INT16_MIN;
    } else {
      return rv;
    }
  }

  FEXCORE_PRESERVE_ALL_ATTR static int32_t handle4t(uint16_t FCW, VectorRegType src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    return extF80_to_i32(&State.State, X80SoftFloat(src), softfloat_round_minMag, false);
  }

  FEXCORE_PRESERVE_ALL_ATTR static int64_t handle8t(uint16_t FCW, VectorRegType src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    return extF80_to_i64(&State.State, X80SoftFloat(src), softfloat_round_minMag, false);
  }
};

template<>
struct OpHandlers<IR::OP_F80CVTTOINT> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle2(uint16_t FCW, int16_t src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return X80SoftFloat(src);
  }

  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle4(uint16_t FCW, int32_t src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return X80SoftFloat(src);
  }
};

template<>
struct OpHandlers<IR::OP_F80ROUND> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FRNDINT(&State.State, Src1);
  }
};

template<>
struct OpHandlers<IR::OP_F80F2XM1> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::F2XM1(&State.State, Src1);
  }
};

template<>
struct OpHandlers<IR::OP_F80TAN> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1)) {
      return X80SoftFloat::Indefinite();
    }
    if (OutOfTrigRange(Src1, Frame)) {
      return Src1;
    }
    return X80SoftFloat::FTAN(&State.State, Src1);
  }
};

template<>
struct OpHandlers<IR::OP_F80SQRT> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    if (InvalidOperand(State, Src1)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FSQRT(&State.State, Src1);
  }
};

template<>
struct OpHandlers<IR::OP_F80SIN> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1)) {
      return X80SoftFloat::Indefinite();
    }
    if (OutOfTrigRange(Src1, Frame)) {
      return Src1;
    }
    return X80SoftFloat::FSIN(&State.State, Src1);
  }
};

template<>
struct OpHandlers<IR::OP_F80COS> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1)) {
      return X80SoftFloat::Indefinite();
    }
    if (OutOfTrigRange(Src1, Frame)) {
      return Src1;
    }
    return X80SoftFloat::FCOS(&State.State, Src1);
  }
};

template<>
struct OpHandlers<IR::OP_F80SINCOS> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegPairType handle(uint16_t FCW, VectorRegType Src1, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1)) {
      return FEXCore::MakeVectorRegPair(X80SoftFloat::Indefinite(), X80SoftFloat::Indefinite());
    }
    // Hardware pushes nothing in this case; the extra stack slot the dispatcher allocates is left
    // holding a copy of the operand.
    if (OutOfTrigRange(Src1, Frame)) {
      return FEXCore::MakeVectorRegPair(Src1, Src1);
    }
    return FEXCore::MakeVectorRegPair(X80SoftFloat::FSIN(&State.State, Src1), X80SoftFloat::FCOS(&State.State, Src1));
  }
};

template<>
struct OpHandlers<IR::OP_F80XTRACT_EXP> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    if (InvalidOperand(State, Src1)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FXTRACT_EXP(Src1);
  }
};

template<>
struct OpHandlers<IR::OP_F80XTRACT_SIG> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    if (InvalidOperand(State, Src1)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FXTRACT_SIG(Src1);
  }
};

template<>
struct OpHandlers<IR::OP_F80ADD> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FADD(&State.State, Src1, Src2);
  }
};

template<>
struct OpHandlers<IR::OP_F80SUB> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FSUB(&State.State, Src1, Src2);
  }
};

template<>
struct OpHandlers<IR::OP_F80MUL> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FMUL(&State.State, Src1, Src2);
  }
};

template<>
struct OpHandlers<IR::OP_F80DIV> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FDIV(&State.State, Src1, Src2);
  }
};

template<>
struct OpHandlers<IR::OP_F80FYL2X> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FYL2X(&State.State, Src1, Src2);
  }
};

template<>
struct OpHandlers<IR::OP_F80FYL2XP1> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FYL2XP1(&State.State, Src1, Src2);
  }
};

template<>
struct OpHandlers<IR::OP_F80ATAN> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FATAN(&State.State, Src1, Src2);
  }
};

template<>
struct OpHandlers<IR::OP_F80FPREM1> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    bool Partial = false;
    uint8_t Quotient = 0;
    const X80SoftFloat Result = X80SoftFloat::FREM(&State.State, Src1, Src2, true, &Partial, &Quotient);
    SetRemainderFlags(Frame, Partial, Quotient);
    return Result;
  }
};

template<>
struct OpHandlers<IR::OP_F80FPREM> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    bool Partial = false;
    uint8_t Quotient = 0;
    const X80SoftFloat Result = X80SoftFloat::FREM(&State.State, Src1, Src2, false, &Partial, &Quotient);
    SetRemainderFlags(Frame, Partial, Quotient);
    return Result;
  }
};

template<>
struct OpHandlers<IR::OP_F80SCALE> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1, VectorRegType Src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    ScopedSoftFloatState State {FCW, Frame, true};
    if (InvalidOperand(State, Src1, Src2)) {
      return X80SoftFloat::Indefinite();
    }
    return X80SoftFloat::FSCALE(&State.State, Src1, Src2);
  }
};

template<>
struct OpHandlers<IR::OP_F64SIN> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return sin(src);
  }
};

template<>
struct OpHandlers<IR::OP_F64COS> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return cos(src);
  }
};

template<>
struct OpHandlers<IR::OP_F64SINCOS> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorScalarF64Pair handle(double src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    double sin, cos;
#ifdef _WIN32
    sin = ::sin(src);
    cos = ::cos(src);
#else
    sincos(src, &sin, &cos);
#endif
    return VectorScalarF64Pair {sin, cos};
  }
};

template<>
struct OpHandlers<IR::OP_F64TAN> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return tan(src);
  }
};

template<>
struct OpHandlers<IR::OP_F64F2XM1> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return exp2(src) - 1.0;
  }
};

template<>
struct OpHandlers<IR::OP_F64ATAN> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src1, double src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return atan2(src1, src2);
  }
};

template<>
struct OpHandlers<IR::OP_F64FPREM> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src1, double src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return fmod(src1, src2);
  }
};

template<>
struct OpHandlers<IR::OP_F64FPREM1> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src1, double src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return remainder(src1, src2);
  }
};

template<>
struct OpHandlers<IR::OP_F64FYL2X> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src1, double src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return src2 * log2(src1);
  }
};

template<>
struct OpHandlers<IR::OP_F64FYL2XP1> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src1, double src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    return src2 * log2(1.0 + src1);
  }
};

template<>
struct OpHandlers<IR::OP_F64SCALE> {
  FEXCORE_PRESERVE_ALL_ATTR static double handle(double src1, double src2, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    if (src1 == 0.0) { // src1 might be +/- zero
      return src1;     // this will return negative or positive zero if when appropriate
    }
    double trun = trunc(src2);
    return src1 * exp2(trun);
  }
};

template<>
struct OpHandlers<IR::OP_F80BCDSTORE> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src1q, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    X80SoftFloat Src1 = Src1q;
    ScopedSoftFloatState State {FCW, Frame};
    bool Negative = Src1.Top.Sign;

    Src1 = X80SoftFloat::FRNDINT(&State.State, Src1);

    // Clear the Sign bit
    Src1.Top.Sign = 0;

    uint64_t Tmp = Src1.ToI64(&State.State);
    X80SoftFloat Rv;
    uint8_t* BCD = reinterpret_cast<uint8_t*>(&Rv);

    for (size_t i = 0; i < 9; ++i) {
      if (Tmp == 0) {
        // Nothing left? Just leave
        break;
      }
      // Extract the lower 100 values
      uint8_t Digit = Tmp % 100;

      // Now divide it for the next iteration
      Tmp /= 100;

      uint8_t UpperNibble = Digit / 10;
      uint8_t LowerNibble = Digit % 10;

      // Now store the BCD
      BCD[i] = (UpperNibble << 4) | LowerNibble;
    }

    // Set negative flag once converted to x87
    BCD[9] = Negative ? 0x80 : 0;

    return Rv;
  }
};

template<>
struct OpHandlers<IR::OP_F80BCDLOAD> {
  FEXCORE_PRESERVE_ALL_ATTR static VectorRegType handle(uint16_t FCW, VectorRegType Src, FEXCore::Core::CpuStateFrame* Frame) {
    FEXCORE_PROFILE_INSTANT_INCREMENT(Frame->Thread, AccumulatedFloatFallbackCount, 1);
    uint8_t* Src1 = reinterpret_cast<uint8_t*>(&Src);
    uint64_t BCD {};
    // We walk through each uint8_t and pull out the BCD encoding
    // Each 4bit split is a digit
    // Only 0-9 is supported, A-F results in undefined data
    // | 4 bit     | 4 bit    |
    // | 10s place | 1s place |
    // EG 0x48 = 48
    // EG 0x4847 = 4847
    // This gives us an 18digit value encoded in BCD
    // The last byte lets us know if it negative or not
    for (size_t i = 0; i < 9; ++i) {
      uint8_t Digit = Src1[8 - i];
      // First shift our last value over
      BCD *= 100;

      // Add the tens place digit
      BCD += (Digit >> 4) * 10;

      // Add the ones place digit
      BCD += Digit & 0xF;
    }

    // Set negative flag once converted to x87
    bool Negative = Src1[9] & 0x80;
    X80SoftFloat Tmp;

    Tmp = BCD;
    Tmp.Top.Sign = Negative;
    return Tmp;
  }
};

} // namespace FEXCore::CPU
