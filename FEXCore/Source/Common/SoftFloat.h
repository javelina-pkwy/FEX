// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/sstream.h>
#include <FEXCore/fextl/string.h>
#include "cephes_128bit.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <stdint.h>

#include "Common/VectorRegType.h"

extern "C" {
#include "SoftFloat-3e/platform.h"
#include "SoftFloat-3e/softfloat.h"

// Internal to SoftFloat-3e (not in its public header): normalizes sig, then rounds and packs the
// value sig * 2^(exp - bias - 63) once, handling overflow, underflow and denormals per state.
FEXCORE_PRESERVE_ALL_ATTR extFloat80_t softfloat_normRoundPackToExtF80(
  struct softfloat_state* state, bool sign, int_fast32_t exp, uint_fast64_t sig, uint_fast64_t sigExtra, uint_fast8_t roundingPrecision);
}

struct FEX_PACKED X80SoftFloat {
#ifdef ARCHITECTURE_x86_64
// Define this to push some operations to x87
// Only useful to see if precision loss is killing something
// #define DEBUG_X86_FLOAT
#ifdef DEBUG_X86_FLOAT
#define BIGFLOAT long double
#define BIGFLOATSIZE 10
#else
#define BIGFLOAT float128_t
#define BIGFLOATSIZE 16
#endif
#elif defined(ARCHITECTURE_arm64)
#define BIGFLOAT float128_t
#define BIGFLOATSIZE 16
#else
#error No 128bit float for this target!
#endif

  uint64_t Significand;
  union {
    uint16_t Raw;
    struct {
      uint16_t Exponent : 15;
      uint16_t Sign     : 1;
    };
  } Top;

  X80SoftFloat() {
    memset(this, 0, sizeof(*this));
  }
  X80SoftFloat(uint16_t _Sign, uint16_t _Exponent, uint64_t _Significand)
    : Significand {_Significand}
    , Top {.Raw = static_cast<uint16_t>((_Exponent & 0x7FFF) | (_Sign << 15))} {}

  fextl::string str() const {
    fextl::ostringstream string;
    string << std::hex << Top.Sign;
    string << "_" << Top.Exponent;
    string << "_" << (Significand >> 63);
    string << "_" << (Significand & ((1ULL << 63) - 1));
    return string.str();
  }

  // Ops
  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FADD(softfloat_state* state, const X80SoftFloat& lhs, const X80SoftFloat& rhs) {
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[rhs]; # st1
    fldt %[lhs]; # st0
    faddp;
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs), [rhs] "m"(rhs)
        : "st", "st(1)");

    return Result;
#else
    return extF80_add(state, Canonical(lhs), Canonical(rhs));
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FSUB(softfloat_state* state, const X80SoftFloat& lhs, const X80SoftFloat& rhs) {
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[rhs]; # st1
    fldt %[lhs]; # st0
    fsubp;
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs), [rhs] "m"(rhs)
        : "st", "st(1)");

    return Result;
#else
    return extF80_sub(state, Canonical(lhs), Canonical(rhs));
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FMUL(softfloat_state* state, const X80SoftFloat& lhs, const X80SoftFloat& rhs) {
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[rhs]; # st1
    fldt %[lhs]; # st0
    fmulp;
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs), [rhs] "m"(rhs)
        : "st", "st(1)");

    return Result;
#else
    return extF80_mul(state, Canonical(lhs), Canonical(rhs));
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FDIV(softfloat_state* state, const X80SoftFloat& lhs, const X80SoftFloat& rhs) {
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[rhs]; # st1
    fldt %[lhs]; # st0
    fdivp;
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs), [rhs] "m"(rhs)
        : "st", "st(1)");

    return Result;
#else
    return extF80_div(state, Canonical(lhs), Canonical(rhs));
#endif
  }

  // FPREM (truncated quotient) and, with Nearest, FPREM1 (round-to-nearest-even quotient).
  // ST(0) - Q * ST(1) is computed exactly on the integer significands. When the exponent
  // difference D is 64 or more only a partial remainder is produced: hardware reduces D to the
  // largest multiple of 32 at least 32 below it (32 to 63 quotient bits), reports it through C2
  // (Partial) and leaves the quotient bits clear. Otherwise Quotient receives the low three bits of
  // Q for C0, C3 and C1.
  // Not preserve_all for the same reason as FYL2XP1.
  [[gnu::noinline]] static X80SoftFloat
  FREM(softfloat_state* state, X80SoftFloat lhs, X80SoftFloat rhs, bool Nearest, bool* Partial, uint8_t* Quotient) {
    *Partial = false;
    *Quotient = 0;
    lhs = Canonical(lhs);
    rhs = Canonical(rhs);
    if (IsNan(lhs) || IsNan(rhs)) {
      return extF80_add(state, lhs, rhs);
    }
    const bool XZero = lhs.Top.Exponent == 0 && lhs.Significand == 0;
    const bool YZero = rhs.Top.Exponent == 0 && rhs.Significand == 0;
    if (lhs.Top.Exponent == 0x7FFF || YZero) {
      state->exceptionFlags |= softfloat_flag_invalid;
      return Indefinite();
    }
    if (XZero || rhs.Top.Exponent == 0x7FFF) {
      return lhs;
    }

    // Normalized significands and the biased exponents of their leading bits, so that
    // value = Sig * 2^(Exp - bias - 63) for denormals too.
    uint64_t SigX = lhs.Significand;
    uint64_t SigY = rhs.Significand;
    int32_t ExpX = lhs.Top.Exponent ? lhs.Top.Exponent : 1;
    int32_t ExpY = rhs.Top.Exponent ? rhs.Top.Exponent : 1;
    const int ShiftX = std::countl_zero(SigX);
    const int ShiftY = std::countl_zero(SigY);
    SigX <<= ShiftX;
    ExpX -= ShiftX;
    SigY <<= ShiftY;
    ExpY -= ShiftY;
    const int32_t D = ExpX - ExpY;

    if (D < -1) {
      return lhs;
    }
    if (D == -1) {
      // |y|/2 <= |x| < |y|: the truncated quotient is 0, the nearest one is 1 above the midpoint
      // (the midpoint itself rounds to the even 0).
      if (!Nearest || SigX <= SigY) {
        return lhs;
      }
      *Quotient = 1;
      X80SoftFloat SignedY = rhs;
      SignedY.Top.Sign = lhs.Top.Sign;
      return extF80_sub(state, lhs, SignedY);
    }

    using u128 = unsigned __int128;
    if (D >= 64) {
      // Hardware reduces the exponent difference to the largest multiple of 32 at least 32 below it,
      // producing 32 to 63 quotient bits per step.
      const int32_t Reduced = 32 * (D / 32 - 1);
      const u128 Num = static_cast<u128>(SigX) << (D - Reduced);
      const uint64_t Rem = static_cast<uint64_t>(Num % SigY);
      *Partial = true;
      if (Rem == 0) {
        return X80SoftFloat(lhs.Top.Sign, 0, 0);
      }
      return softfloat_normRoundPackToExtF80(state, lhs.Top.Sign, ExpY + Reduced, Rem, 0, state->roundingPrecision);
    }

    const u128 Num = static_cast<u128>(SigX) << D;
    uint64_t Q = static_cast<uint64_t>(Num / SigY);
    uint64_t Rem = static_cast<uint64_t>(Num % SigY);
    bool Negative = false;
    if (Nearest) {
      // Rounding the quotient up makes the remainder negative.
      const u128 Twice = static_cast<u128>(Rem) * 2;
      if (Twice > SigY || (Twice == SigY && (Q & 1))) {
        ++Q;
        Rem = SigY - Rem;
        Negative = true;
      }
    }
    *Quotient = Q & 7;
    const bool Sign = lhs.Top.Sign ^ Negative;
    if (Rem == 0) {
      return X80SoftFloat(Sign, 0, 0);
    }
    return softfloat_normRoundPackToExtF80(state, Sign, ExpY, Rem, 0, state->roundingPrecision);
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FRNDINT(softfloat_state* state, const X80SoftFloat& lhs) {
    return extF80_roundToInt(state, Canonical(lhs), state->roundingMode, true);
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FRNDINT(softfloat_state* state, const X80SoftFloat& lhs, uint_fast8_t RoundMode) {
    return extF80_roundToInt(state, Canonical(lhs), RoundMode, true);
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FXTRACT_SIG(X80SoftFloat lhs) {
    lhs = Canonical(lhs);
#if defined(DEBUG_X86_FLOAT)
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[lhs]; # st0
    fxtract;
    fstpt %[result];
    ffreep %%st(0);
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs)
        : "st", "st(1)");

    return Result;
#else
    // Zero is a special case, the significand for +/- 0 is +/- zero.
    if (lhs.Top.Exponent == 0x0 && lhs.Significand == 0x0) {
      return lhs;
    }
    // Inf/NaN pass through unchanged in the significand slot.
    if (lhs.Top.Exponent == 0x7FFF) {
      return lhs;
    }
    X80SoftFloat Tmp = lhs;
    Tmp.Top.Exponent = 0x3FFF;
    Tmp.Top.Sign = lhs.Top.Sign;
    return Tmp;
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FXTRACT_EXP(X80SoftFloat lhs) {
    lhs = Canonical(lhs);
#if defined(DEBUG_X86_FLOAT)
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[lhs]; # st0
    fxtract;
    ffreep %%st(0);
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs)
        : "st", "st(1)");

    return Result;
#else
    // Zero is a special case, the exponent is always -inf
    if (lhs.Top.Exponent == 0x0 && lhs.Significand == 0x0) {
      X80SoftFloat Result(1, 0x7FFFUL, 0x8000'0000'0000'0000UL);
      return Result;
    }
    // +/-Inf returns +Inf in the exponent slot; NaN propagates.
    if (lhs.Top.Exponent == 0x7FFF) {
      if ((lhs.Significand & 0x7FFFFFFFFFFFFFFFULL) == 0) {
        X80SoftFloat Result(0, 0x7FFFUL, 0x8000'0000'0000'0000UL);
        return Result;
      }
      return lhs;
    }

    int32_t TrueExp = lhs.Top.Exponent - ExponentBias;
    return i32_to_extF80(TrueExp);
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static void FCMP(softfloat_state* state, X80SoftFloat lhs, X80SoftFloat rhs, bool* eq, bool* lt, bool* nan) {
    lhs = Canonical(lhs);
    rhs = Canonical(rhs);
    // A signaling NaN operand raises invalid regardless of the compare flavour; quiet NaNs are left
    // to the caller (FCOM signals on them, FUCOM does not), so the predicates below must not raise.
    if (IsNan(lhs) || IsNan(rhs)) {
      if (IsSignalingNan(lhs) || IsSignalingNan(rhs)) {
        state->exceptionFlags |= softfloat_flag_invalid;
      }
      *eq = false;
      *lt = false;
      *nan = true;
      return;
    }
    *eq = extF80_eq(state, lhs, rhs);
    *lt = extF80_lt(state, lhs, rhs);
    *nan = false;
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FSCALE(softfloat_state* state, X80SoftFloat lhs, X80SoftFloat rhs) {
    lhs = Canonical(lhs);
    rhs = Canonical(rhs);
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[rhs]; # st1
    fldt %[lhs]; # st0
    fscale; # st0 = st0 * 2^(rdint(st1))
    fstpt %[result];
    ffreep %%st(0);
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs), [rhs] "m"(rhs)
        : "st", "st(1)");

    return Result;
#else
    // ST(0) * 2^trunc(ST(1)). NaNs propagate; extF80_mul raises invalid only for a signaling one.
    if (IsNan(lhs) || IsNan(rhs)) {
      return extF80_mul(state, lhs, rhs);
    }
    const bool XZero = lhs.Top.Exponent == 0 && lhs.Significand == 0;
    const bool XInf = lhs.Top.Exponent == 0x7FFF;
    if (rhs.Top.Exponent == 0x7FFF) {
      // 0 * 2^+inf and inf * 2^-inf are invalid. Otherwise an infinite scale yields a signed infinity
      // or zero exactly, without the overflow rounding a finite scale would go through.
      if ((XZero && !rhs.Top.Sign) || (XInf && rhs.Top.Sign)) {
        state->exceptionFlags |= softfloat_flag_invalid;
        return Indefinite();
      }
      if (XZero || XInf) {
        return lhs;
      }
      return rhs.Top.Sign ? X80SoftFloat(lhs.Top.Sign, 0, 0) : X80SoftFloat(lhs.Top.Sign, 0x7FFF, IntegerBit);
    }
    if (XZero || XInf) {
      return lhs;
    }

    // A |ST(1)| of 2^16 or more scales past both ends of the exponent range; saturating the truncated
    // scale keeps the exponent arithmetic in range without changing the result.
    int_fast32_t Scale;
    if (rhs.Top.Exponent >= ExponentBias + 16) {
      Scale = rhs.Top.Sign ? -65536 : 65536;
    } else {
      Scale = extF80_to_i32(state, rhs, softfloat_round_minMag, false);
    }

    // The scaled value is exact, so a single round-and-pack of the shifted exponent gives the
    // overflow, underflow and denormal behaviour of the control word. A denormal ST(0) has the
    // biased exponent of 1 its significand encoding implies.
    const int_fast32_t Exponent = (lhs.Top.Exponent == 0 ? 1 : lhs.Top.Exponent) + Scale;
    return softfloat_normRoundPackToExtF80(state, lhs.Top.Sign, Exponent, lhs.Significand, 0, state->roundingPrecision);
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat F2XM1(softfloat_state* state, const X80SoftFloat& lhs) {
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[lhs]; # st0
    f2xm1; # st0 = 2^st(0) - 1
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs)
        : "st");

    return Result;
#else
    // NaNs propagate (a signaling one raises invalid), zero keeps its sign, 2^(+inf)-1 is +inf and
    // 2^(-inf)-1 is -1.
    if (IsNan(lhs)) {
      return extF80_add(state, lhs, lhs);
    }
    if (lhs.Top.Exponent == 0 && lhs.Significand == 0) {
      return lhs;
    }
    if (lhs.Top.Exponent == 0x7FFF) {
      return lhs.Top.Sign ? X80SoftFloat(1, 0x3FFF, 0x8000000000000000ULL) : lhs;
    }
    // Outside [-1, 1] the SDM leaves the result undefined; the AMD hardware used as reference returns
    // the argument unchanged and reports it inexact.
    if (lhs.Top.Exponent > ExponentBias || (lhs.Top.Exponent == ExponentBias && lhs.Significand != IntegerBit)) {
      return MarkInexact(state, lhs);
    }

    auto Src1_d = Canonical(lhs).ToFMax(state);
    static const float128_t one {0x0ULL, 0x3fff000000000000ULL};

    // exp2(x) - 1 cancels catastrophically once |x| is small: exp2(x) rounds to 1 while the true
    // result is x*ln2 + O(x^2). Below 2^-32 evaluate the Taylor series t + t^2/2 + t^3/6 with
    // t = x*ln2 instead; the omitted t^4/24 term is around 2^-100 relative, far below the target.
    // Intermediate binary128 arithmetic runs on a scratch state; only the final 80-bit rounding
    // reports flags.
    softfloat_state Scratch = *state;
    if (lhs.Top.Exponent < ExponentBias - 32) {
      static const float128_t half {0x0ULL, 0x3ffe000000000000ULL};
      static const float128_t sixth {0x5555555555555555ULL, 0x3FFC555555555555ULL};
      const float128_t t = f128_mul(&Scratch, Src1_d, Ln2_128);
      float128_t poly = f128_add(&Scratch, half, f128_mul(&Scratch, t, sixth));
      poly = f128_add(&Scratch, one, f128_mul(&Scratch, t, poly));
      float128_t Result = f128_mul(&Scratch, t, poly);
      // 2^x - 1 > x*ln2 for either sign of x: keep the direction when the quadratic term is below
      // binary128 precision.
      if (SameBits(Result, t)) {
        Result = NudgeToward(Result, true);
      }
      return MarkInexact(state, X80SoftFloat(state, Result));
    }

    auto Result = FEXCore::cephes_128bit::exp2l(Src1_d);
    return MarkInexact(state, FromF128Result(state, f128_sub(&Scratch, Result, one)));
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FYL2X(softfloat_state* state, X80SoftFloat lhs, X80SoftFloat rhs) {
    lhs = Canonical(lhs);
    rhs = Canonical(rhs);
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[rhs]; # st(1)
    fldt %[lhs]; # st(0)
    fyl2x; # st(1) * log2l(st(0))
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs), [rhs] "m"(rhs)
        : "st", "st(1)");

    return Result;
#else
    // ST(1) * log2(ST(0)) special cases: NaNs propagate; log2 of a negative number is invalid; log2(0)
    // is -inf (zero-divide) and flips with the sign of ST(1) unless ST(1) is also zero, which is
    // invalid; log2(+inf) is +inf unless ST(1) is zero; and inf * log2(1) = inf * 0 is invalid.
    // cephes clamps log2 of 0/inf/NaN to +-16384 instead, so these must be handled here.
    if (IsNan(lhs) || IsNan(rhs)) {
      return extF80_add(state, lhs, rhs);
    }
    const bool XZero = lhs.Top.Exponent == 0 && lhs.Significand == 0;
    const bool YZero = rhs.Top.Exponent == 0 && rhs.Significand == 0;
    const bool XInf = lhs.Top.Exponent == 0x7FFF;
    const bool YInf = rhs.Top.Exponent == 0x7FFF;
    const bool XOne = lhs.Top.Exponent == ExponentBias && lhs.Significand == IntegerBit;
    if ((lhs.Top.Sign && !XZero) || (XZero && YZero) || (XInf && YZero) || (XOne && YInf)) {
      state->exceptionFlags |= softfloat_flag_invalid;
      return Indefinite();
    }
    if (XZero) {
      // log2(0) is a zero-divide; hardware does not report it when ST(1) is already infinite.
      if (!YInf) {
        state->exceptionFlags |= softfloat_flag_infinite;
      }
      return X80SoftFloat(!rhs.Top.Sign, 0x7FFF, IntegerBit);
    }
    if (XInf) {
      return X80SoftFloat(rhs.Top.Sign, 0x7FFF, IntegerBit);
    }
    if (YInf) {
      // inf * log2(x) is an exact infinity, negative when x < 1.
      const bool LessThanOne = lhs.Top.Exponent < ExponentBias;
      return X80SoftFloat(rhs.Top.Sign ^ LessThanOne, 0x7FFF, IntegerBit);
    }

    auto Src1_d = lhs.ToFMax(state);
    auto Src2_d = rhs.ToFMax(state);

    // The final product runs on the real state so that an underflow to zero inside binary128 is
    // still reported.
    auto Tmp = f128_mul(state, Src2_d, FEXCore::cephes_128bit::log2l(Src1_d));
    return MarkInexact(state, FromF128Result(state, Tmp));
#endif
  }

  // Deliberately not preserve_all: this keeps enough values live across the softfloat calls that
  // clang's preserve_all frame lowering runs past the stp offset range and spills callee-saved
  // registers below the stack pointer, where the nested calls destroy them. The preserve_all fallback
  // handler calling this saves the register file around the call instead.
  [[gnu::noinline]] static X80SoftFloat FYL2XP1(softfloat_state* state, X80SoftFloat lhs, X80SoftFloat rhs) {
    lhs = Canonical(lhs);
    rhs = Canonical(rhs);
    // ST(1) * log2(1 + ST(0)). NaNs propagate. A zero ST(0) gives a zero signed by the product of the
    // operand signs (invalid against an infinite ST(1)); an infinite ST(1) gives an infinity signed
    // the same way. ST(0) below -1 is invalid, exactly -1 is log2(0): a zero-divide infinity.
    if (IsNan(lhs) || IsNan(rhs)) {
      return extF80_add(state, lhs, rhs);
    }
    const bool XZero = lhs.Top.Exponent == 0 && lhs.Significand == 0;
    const bool YZero = rhs.Top.Exponent == 0 && rhs.Significand == 0;
    const bool XInf = lhs.Top.Exponent == 0x7FFF;
    const bool YInf = rhs.Top.Exponent == 0x7FFF;
    const uint16_t ResultSign = lhs.Top.Sign ^ rhs.Top.Sign;
    // ST(0) <= -1 is outside the defined range. The AMD hardware used as reference returns ST(0)
    // unchanged and inexact, or a zero signed by the product of the signs when ST(1) is zero; a
    // negative infinity is invalid.
    const bool XBelowMinusOne = lhs.Top.Sign && !XZero && !XInf && lhs.Top.Exponent >= ExponentBias;
    if ((XZero && YInf) || (XInf && (YZero || lhs.Top.Sign))) {
      state->exceptionFlags |= softfloat_flag_invalid;
      return Indefinite();
    }
    if (XBelowMinusOne) {
      if (YZero) {
        return X80SoftFloat(ResultSign, 0, 0);
      }
      if (YInf) {
        return X80SoftFloat(ResultSign, 0x7FFF, IntegerBit);
      }
      return MarkInexact(state, lhs);
    }
    if (XZero) {
      return X80SoftFloat(ResultSign, 0, 0);
    }
    if (XInf || YInf) {
      return X80SoftFloat(ResultSign, 0x7FFF, IntegerBit);
    }

    // Forming 1 + x directly loses the low bits of a small x (and all of it below 2^-113), which is
    // exactly the range this instruction exists for. Correct the rounding of u = 1 + x with
    // log2(1 + x) = log2(u) * (x / (u - 1)), and fall back to x / ln2 when u rounds to 1. The ratio
    // is formed first because log2(u) * x overflows for x near the largest finite value.
    static const float128_t one {0x0ULL, 0x3fff000000000000ULL};
    softfloat_state Scratch = *state;
    const float128_t X = lhs.ToFMax(state);
    const float128_t U = f128_add(&Scratch, one, X);
    float128_t Log;
    if (U.v[0] == one.v[0] && U.v[1] == one.v[1]) {
      // log2(1 + x) < x/ln2 for either sign of x once the quadratic term is below binary128 precision.
      Log = NudgeToward(f128_div(&Scratch, X, Ln2_128), false);
    } else {
      Log = f128_mul(&Scratch, FEXCore::cephes_128bit::log2l(U), f128_div(&Scratch, X, f128_sub(&Scratch, U, one)));
    }
    // The final product runs on the real state so that an underflow to zero inside binary128 is
    // still reported.
    return MarkInexact(state, FromF128Result(state, f128_mul(state, rhs.ToFMax(state), Log)));
  }

  // Not preserve_all for the same reason as FYL2XP1.
  [[gnu::noinline]] static X80SoftFloat FATAN(softfloat_state* state, X80SoftFloat lhs, X80SoftFloat rhs) {
    lhs = Canonical(lhs);
    rhs = Canonical(rhs);
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[lhs];
    fldt %[rhs];
    fpatan;
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs), [rhs] "m"(rhs)
        : "st", "st(1)");

    return Result;
#else
    // atan(ST(1) / ST(0)) with lhs = ST(1) = y and rhs = ST(0) = x. Every special result in the SDM
    // operand table is a multiple of pi carrying the sign of y: zero for x >= +0 on the x axis, pi
    // for x <= -0, pi/2 on the y axis or for an infinite y against a finite x, and pi/4 or 3pi/4 for
    // infinities in both. cephes atan2l gets several of these wrong (atan2(-0, -0) came out +0 and
    // atan2(+inf, +inf) NaN), so only the finite non-zero case is left to it.
    if (IsNan(lhs) || IsNan(rhs)) {
      return extF80_add(state, lhs, rhs);
    }
    const bool YZero = lhs.Top.Exponent == 0 && lhs.Significand == 0;
    const bool XZero = rhs.Top.Exponent == 0 && rhs.Significand == 0;
    const bool YInf = lhs.Top.Exponent == 0x7FFF;
    const bool XInf = rhs.Top.Exponent == 0x7FFF;
    const bool YNegative = lhs.Top.Sign;
    const bool XNegative = rhs.Top.Sign;
    if (YZero || XInf) {
      if (YInf) {
        static constexpr float128_t ThreeQuarters {0x0ULL, 0x3FFE800000000000ULL};
        softfloat_state Scratch = *state;
        return FromF128(state, XNegative ? f128_mul(&Scratch, Pi_128, ThreeQuarters) : QuarterPi_128, YNegative);
      }
      if (XNegative) {
        return FromF128(state, Pi_128, YNegative);
      }
      return X80SoftFloat(YNegative, 0, 0);
    }
    if (XZero || YInf) {
      return FromF128(state, HalfPi_128, YNegative);
    }

    // For a tiny |y/x| with x > 0, atan(r) = r(1 - r^2/3) with the cubic term far below precision:
    // round the ratio of the normalized operands toward zero at its true scale in one step. This also
    // covers denormal operands, which cephes would flush.
    if (!XNegative) {
      int32_t ShiftY = 0;
      int32_t ShiftX = 0;
      const float128_t UnitY = NormalizeToUnit(lhs, &ShiftY);
      const float128_t UnitX = NormalizeToUnit(rhs, &ShiftX);
      if (ShiftY - ShiftX < -64) {
        softfloat_state Scratch = *state;
        return MarkInexact(state, FromF128Scaled(state, NudgeTowardZero(f128_div(&Scratch, UnitY, UnitX)), ShiftY - ShiftX));
      }
    }

    BIGFLOAT Src1_d = lhs.ToFMax(state);
    BIGFLOAT Src2_d = rhs.ToFMax(state);
    BIGFLOAT Tmp = FEXCore::cephes_128bit::atan2l(Src1_d, Src2_d);
    // |atan(y/x)| < |y/x|: keep the direction when the cubic term is below binary128 precision. The
    // ratio is formed on a scratch state so its rounding does not leak into the flags.
    softfloat_state Scratch = *state;
    if (SameBits(Tmp, f128_div(&Scratch, Src1_d, Src2_d))) {
      Tmp = NudgeTowardZero(Tmp);
    }
    return MarkInexact(state, X80SoftFloat(state, Tmp));
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FTAN(softfloat_state* state, X80SoftFloat lhs) {
    lhs = Canonical(lhs);
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[lhs]; # st0
    fptan;
    ffreep %%st(0);
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs)
        : "st");

    return Result;
#else
    X80SoftFloat result;
    if (HandleInfinityOp(state, lhs, result)) {
      return result;
    }

    if (IsNan(lhs)) {
      return extF80_add(state, lhs, lhs);
    }
    if (lhs.Top.Exponent == 0 && lhs.Significand == 0) {
      return lhs;
    }

    const BIGFLOAT X = lhs.ToFMax(state);
    BIGFLOAT Src_d = FEXCore::cephes_128bit::tanl(X);
    // |tan x| > |x|: keep the direction when the cubic term is below binary128 precision.
    if (SameBits(Src_d, X)) {
      Src_d = NudgeAwayFromZero(Src_d);
    }
    return MarkInexact(state, X80SoftFloat(state, Src_d));
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FSIN(softfloat_state* state, X80SoftFloat lhs) {
    lhs = Canonical(lhs);
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[lhs]; # st0
    fsin;
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs)
        : "st");

    return Result;
#else
    X80SoftFloat result;
    if (HandleInfinityOp(state, lhs, result)) {
      return result;
    }

    // NaNs propagate and sin(+-0) is +-0; cephes drops the sign of a zero argument.
    if (IsNan(lhs)) {
      return extF80_add(state, lhs, lhs);
    }
    if (lhs.Top.Exponent == 0 && lhs.Significand == 0) {
      return lhs;
    }

    const BIGFLOAT X = lhs.ToFMax(state);
    BIGFLOAT Src_d = FEXCore::cephes_128bit::sinl(X);
    // |sin x| < |x|: keep the direction when the cubic term is below binary128 precision.
    if (SameBits(Src_d, X)) {
      Src_d = NudgeTowardZero(Src_d);
    }
    return MarkInexact(state, X80SoftFloat(state, Src_d));
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FCOS(softfloat_state* state, X80SoftFloat lhs) {
    lhs = Canonical(lhs);
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[lhs]; # st0
    fcos;
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs)
        : "st");

    return Result;
#else
    X80SoftFloat result;
    if (HandleInfinityOp(state, lhs, result)) {
      return result;
    }

    if (IsNan(lhs)) {
      return extF80_add(state, lhs, lhs);
    }

    // cos(+-0) is exactly 1.
    if (lhs.Top.Exponent == 0 && lhs.Significand == 0) {
      return X80SoftFloat(0, ExponentBias, IntegerBit);
    }

    BIGFLOAT Src_d = FEXCore::cephes_128bit::cosl(lhs.ToFMax(state));
    // cos x < 1 for x != 0: keep the direction when the quadratic term is below binary128 precision.
    static const float128_t one {0x0ULL, 0x3fff000000000000ULL};
    if (SameBits(Src_d, one)) {
      Src_d = NudgeTowardZero(Src_d);
    }
    return MarkInexact(state, X80SoftFloat(state, Src_d));
#endif
  }

  FEXCORE_PRESERVE_ALL_ATTR static X80SoftFloat FSQRT(softfloat_state* state, const X80SoftFloat& lhs) {
#ifdef DEBUG_X86_FLOAT
    BIGFLOAT Result;
    asm(R"(
    fninit;
    fldt %[lhs]; # st0
    fsqrt;
    fstpt %[result];
    )"
        : [result] "=m"(Result)
        : [lhs] "m"(lhs)
        : "st");

    return Result;
#else
    return extF80_sqrt(state, Canonical(lhs));
#endif
  }

  float ToF32(softfloat_state* state) const {
    const float32_t Result = extF80_to_f32(state, *this);
    return std::bit_cast<float>(Result);
  }

  double ToF64(softfloat_state* state) const {
    const float64_t Result = extF80_to_f64(state, *this);
    return std::bit_cast<double>(Result);
  }

  FEXCore::VectorRegType ToVector() const {
    FEXCore::VectorRegType Ret {};
    memcpy(&Ret, this, sizeof(*this));
    return Ret;
  }

  BIGFLOAT ToFMax(softfloat_state* state) const {
#if BIGFLOATSIZE == 16
    const float128_t Result = extF80_to_f128(state, *this);
    return std::bit_cast<BIGFLOAT>(Result);
#else
    BIGFLOAT result {};
    memcpy(&result, this, sizeof(result));
    return result;
#endif
  }

  int16_t ToI16(softfloat_state* state) const {
    auto rv = extF80_to_i32(state, *this, state->roundingMode, true);
    if (rv > INT16_MAX || rv < INT16_MIN) {
      ///< Indefinite value for 16-bit conversions.
      return INT16_MIN;
    } else {
      return rv;
    }
  }

  int32_t ToI32(softfloat_state* state) const {
    return extF80_to_i32(state, *this, state->roundingMode, true);
  }

  int64_t ToI64(softfloat_state* state) const {
    return extF80_to_i64(state, *this, state->roundingMode, true);
  }

  uint64_t ToUI64(softfloat_state* state) const {
    return extF80_to_ui64(state, *this, state->roundingMode, false);
  }

  void operator=(const int16_t rhs) {
    *this = i32_to_extF80(rhs);
  }

  void operator=(const int32_t rhs) {
    *this = i32_to_extF80(rhs);
  }

  void operator=(const uint64_t rhs) {
    *this = ui64_to_extF80(rhs);
  }

#if BIGFLOATSIZE == 10
  void operator=(const long double rhs) {
    memcpy(this, &rhs, sizeof(rhs));
  }
#endif

  operator void*() {
    return reinterpret_cast<void*>(this);
  }

  X80SoftFloat(extFloat80_t rhs) {
    Significand = rhs.signif;
    Top.Raw = rhs.signExp;
  }

  X80SoftFloat(softfloat_state* state, const float rhs) {
    *this = f32_to_extF80(state, std::bit_cast<float32_t>(rhs));
  }

  X80SoftFloat(softfloat_state* state, const double rhs) {
    *this = f64_to_extF80(state, std::bit_cast<float64_t>(rhs));
  }

  X80SoftFloat(softfloat_state* state, BIGFLOAT rhs) {
#if BIGFLOATSIZE == 16
    *this = f128_to_extF80(state, std::bit_cast<float128_t>(rhs));
#else
    *this = std::bit_cast<long double>(rhs);
#endif
  }

  X80SoftFloat(const int16_t rhs) {
    *this = i32_to_extF80(rhs);
  }

  X80SoftFloat(const int32_t rhs) {
    *this = i32_to_extF80(rhs);
  }

  X80SoftFloat(const FEXCore::VectorRegType rhs) {
    memcpy(this, &rhs, sizeof(*this));
  }

  void operator=(extFloat80_t rhs) {
    Significand = rhs.signif;
    Top.Raw = rhs.signExp;
  }

  operator FEXCore::VectorRegType() const {
    return ToVector();
  }

  operator extFloat80_t() const {
    extFloat80_t Result {};
    Result.signif = Significand;
    Result.signExp = Top.Raw;
    return Result;
  }

  // Any NaN with the integer bit set, including the default QNaN whose only payload bit is the quiet
  // bit (0xC000'0000'0000'0000).
  static bool IsNan(const X80SoftFloat& lhs) {
    return (lhs.Top.Exponent == 0x7FFF) && (lhs.Significand & IntegerBit) && (lhs.Significand & ~IntegerBit);
  }

  static bool IsSignalingNan(const X80SoftFloat& lhs) {
    return IsNan(lhs) && !(lhs.Significand & QuietBit);
  }

  // FSIN/FCOS/FSINCOS/FPTAN only reduce arguments below 2^63 in magnitude; beyond that they set C2
  // and leave the operand untouched.
  static bool IsOutOfTrigRange(const X80SoftFloat& lhs) {
    return lhs.Top.Exponent != 0x7FFF && lhs.Top.Exponent >= ExponentBias + 63;
  }

  // Pseudo-NaN, pseudo-infinity (exponent all ones, integer bit clear) and unnormal (any other
  // non-zero exponent, integer bit clear) encodings. The x87 treats these as invalid operands.
  static bool IsUnsupported(const X80SoftFloat& lhs) {
    return lhs.Top.Exponent != 0 && !(lhs.Significand & IntegerBit);
  }

  // The QNaN floating-point indefinite produced by a masked invalid operation.
  static X80SoftFloat Indefinite() {
    return X80SoftFloat(1, 0x7FFF, 0xC000000000000000ULL);
  }

  // A pseudo-denormal (zero exponent, integer bit set) denotes the same value as the normal with
  // biased exponent 1. Berkeley SoftFloat assumes subnormal significands have the integer bit clear,
  // so it orders pseudo-denormals lexicographically below that normal and drops the carry when
  // adding two of them; hardware evaluates them by value.
  static X80SoftFloat Canonical(const X80SoftFloat& lhs) {
    if (lhs.Top.Exponent != 0 || !(lhs.Significand & IntegerBit)) {
      return lhs;
    }
    X80SoftFloat Result = lhs;
    Result.Top.Exponent = 1;
    return Result;
  }

  static bool SignBit(const X80SoftFloat& lhs) {
    return lhs.Top.Sign;
  }

private:
  static constexpr uint64_t IntegerBit = (1ULL << 63);
  static constexpr uint64_t QuietBit = (1ULL << 62);
  // ln(2), pi, pi/2 and pi/4 as binary128.
  static constexpr float128_t Ln2_128 {0xF35793C7673007E6ULL, 0x3FFE62E42FEFA39EULL};
  static constexpr float128_t Pi_128 {0x8469898CC51701B8ULL, 0x4000921FB54442D1ULL};
  static constexpr float128_t HalfPi_128 {0x8469898CC51701B8ULL, 0x3FFF921FB54442D1ULL};
  static constexpr float128_t QuarterPi_128 {0x8469898CC51701B8ULL, 0x3FFE921FB54442D1ULL};

  // Rounds a binary128 magnitude to 80 bits with the requested sign applied first, so directed
  // rounding modes see the signed value.
  static X80SoftFloat FromF128(softfloat_state* state, float128_t Value, bool Negative) {
    if (Negative) {
      Value.v[1] ^= 0x8000000000000000ULL;
    }
    return X80SoftFloat(state, Value);
  }

  static bool SameBits(float128_t a, float128_t b) {
    return a.v[0] == b.v[0] && a.v[1] == b.v[1];
  }

  // Rounds a binary128 result to 80 bits. A product of finite operands that overflowed binary128 is
  // an infinity there, which would convert exactly; route it through the overflow response instead
  // so the control word decides between infinity and the largest finite value and OE/PE are raised.
  static X80SoftFloat FromF128Result(softfloat_state* state, float128_t Value) {
    const bool Infinite = ((Value.v[1] >> 48) & 0x7FFF) == 0x7FFF && (Value.v[1] & 0xFFFFFFFFFFFFULL) == 0 && Value.v[0] == 0;
    if (Infinite) {
      return softfloat_normRoundPackToExtF80(state, Value.v[1] >> 63, 0x7FFF, IntegerBit, 0, state->roundingPrecision);
    }
    return X80SoftFloat(state, Value);
  }

  // Hardware evaluates the transcendentals iteratively and reports every finite non-zero result as
  // inexact, even one that happens to be exactly representable (log2 of a power of two).
  static X80SoftFloat MarkInexact(softfloat_state* state, X80SoftFloat Result) {
    const bool Zero = Result.Top.Exponent == 0 && Result.Significand == 0;
    if (!Zero && Result.Top.Exponent != 0x7FFF) {
      state->exceptionFlags |= softfloat_flag_inexact;
      // An inexact tiny result is an underflow as well.
      if (Result.Top.Exponent == 0) {
        state->exceptionFlags |= softfloat_flag_underflow;
      }
    }
    return Result;
  }

  // Value = Unit * 2^Shift with Unit in [1, 2) as a normal binary128. cephes flushes binary128
  // denormals, so operands below 2^-16382 are evaluated in this normalized form instead.
  static float128_t NormalizeToUnit(const X80SoftFloat& Value, int32_t* Shift) {
    uint64_t Sig = Value.Significand;
    int32_t Exp = Value.Top.Exponent ? Value.Top.Exponent : 1;
    const int LeadingZeros = std::countl_zero(Sig);
    Sig <<= LeadingZeros;
    Exp -= LeadingZeros;
    *Shift = Exp - ExponentBias;
    softfloat_state Exact {};
    return X80SoftFloat(Value.Top.Sign, ExponentBias, Sig).ToFMax(&Exact);
  }

  // Rounds Value * 2^Shift to 80 bits in a single step for a finite normal binary128 Value, so a
  // result in the denormal range or beyond the exponent range rounds exactly once.
  static X80SoftFloat FromF128Scaled(softfloat_state* state, float128_t Value, int32_t Shift) {
    const bool Sign = Value.v[1] >> 63;
    const int32_t Exp = (Value.v[1] >> 48) & 0x7FFF;
    const unsigned __int128 Mantissa = (static_cast<unsigned __int128>((Value.v[1] & 0xFFFFFFFFFFFFULL) | (1ULL << 48)) << 64) | Value.v[0];
    // 113 significant bits moved to the top of 128 so that sig carries the leading 64.
    const unsigned __int128 Shifted = Mantissa << 15;
    return softfloat_normRoundPackToExtF80(state, Sign, Exp + Shift, static_cast<uint64_t>(Shifted >> 64), static_cast<uint64_t>(Shifted),
                                           state->roundingPrecision);
  }

  // Moves a finite non-zero binary128 value one ulp toward +inf or -inf. When a transcendental
  // result rounds to its leading term in binary128 although the true value lies strictly on one
  // side of it (sin x rounding to x, cos x to 1), the 80-bit rounding would otherwise see an exact
  // value and directed rounding modes would produce the wrong neighbour.
  static float128_t NudgeToward(float128_t Value, bool PlusInfinity) {
    const bool Negative = Value.v[1] >> 63;
    const bool Up = PlusInfinity != Negative;
    unsigned __int128 Bits = (static_cast<unsigned __int128>(Value.v[1] & 0x7FFFFFFFFFFFFFFFULL) << 64) | Value.v[0];
    if (Bits == 0) {
      // The leading term itself underflowed; there is no neighbour to move to.
      return Value;
    }
    Bits += Up ? 1 : static_cast<unsigned __int128>(-1);
    Value.v[0] = static_cast<uint64_t>(Bits);
    Value.v[1] = (Value.v[1] & 0x8000000000000000ULL) | static_cast<uint64_t>(Bits >> 64);
    return Value;
  }

  static float128_t NudgeTowardZero(float128_t Value) {
    return NudgeToward(Value, Value.v[1] >> 63);
  }

  static float128_t NudgeAwayFromZero(float128_t Value) {
    return NudgeToward(Value, !(Value.v[1] >> 63));
  }
  static constexpr uint64_t Bottom62Significand = ((1ULL << 62) - 1);
  static constexpr uint32_t ExponentBias = 16383;

  // Helper function to check for infinity and set invalid operation flag.
  // Returns true if infinity is dealt with, false otherwise.
  FEXCORE_PRESERVE_ALL_ATTR static bool HandleInfinityOp(softfloat_state* state, const X80SoftFloat& arg, X80SoftFloat& result) {
    if (arg.Top.Exponent == 0x7FFF && arg.Significand == 0x8000000000000000ULL) {
      state->exceptionFlags |= softfloat_flag_invalid;
      // Return QNaN.
      result.Top.Sign = 0;
      result.Top.Exponent = 0x7FFF;
      result.Significand = 0xC000000000000000ULL;
      return true;
    }
    return false;
  }
};

static_assert(sizeof(X80SoftFloat) == 10, "tword must be 10bytes in size");
