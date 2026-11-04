// platform/nativeee/native -- included at the head of every game translation unit of the Vita's native lane by
// tools/te_cxx_arm.py, whose rewrite R8 turns the game's casts to integer types, static_cast<T>(x), into te_int_cast<T>(x).
//
// A float cast to an integer type it does not fit is undefined in C++, and each compiler answers it its own way. The wasm
// module (clang for wasm32 without the non-trapping conversions: the desktop's and the Vita module lane's game code) guards
// every such conversion as LLVM lowers it -- s32: |x| < 2^31 ? the truncation : INT_MIN (a NaN too); u32: 0 <= x < 2^32 ?
// the truncation : 0; s64 / u64 the same at 2^63 / 2^64; the 8- and 16-bit types through the s32 guard, then their low bits
// -- while the Cortex-A9's VCVT saturates (INT_MAX for a large positive value, 0 for a NaN). These are the module's answers,
// so the native game makes the module's bits: OLEG's anchored sprite widgets never set their wobble base, and the garbage
// left there (1.6e34 for the HUD's lives head) made an angle of INT_MIN in the module and INT_MAX natively, the head turned
// by -2.8 degrees on one lane and +2.8 on the other. In range, and for every operand that is not a float or a double, the
// cast is the one written.
//
// No static_cast to an integer type in this file: R8 would turn it into a call of itself.
#pragma once

template <class To, class From>
constexpr To te_int_cast(From value)
{
    return (To)value;
}

template <class To>
constexpr To te_int_cast(float value)
{
    if constexpr (sizeof(To) == 8)
    {
        if constexpr ((To)-1 < (To)0)
        {
            return __builtin_fabsf(value) < 0x1p63f ? (To)value : (To)(-0x7FFFFFFFFFFFFFFFLL - 1);
        }
        else
        {
            return value < 0x1p64f && value >= 0.0f ? (To)value : (To)0;
        }
    }
    else if constexpr (sizeof(To) == 4 && (To)-1 > (To)0)
    {
        return value < 0x1p32f && value >= 0.0f ? (To)value : (To)0;
    }
    else
    {
        return (To)(__builtin_fabsf(value) < 0x1p31f ? (int)value : (int)(-0x7FFFFFFF - 1));
    }
}

template <class To>
constexpr To te_int_cast(double value)
{
    if constexpr (sizeof(To) == 8)
    {
        if constexpr ((To)-1 < (To)0)
        {
            return __builtin_fabs(value) < 0x1p63 ? (To)value : (To)(-0x7FFFFFFFFFFFFFFFLL - 1);
        }
        else
        {
            return value < 0x1p64 && value >= 0.0 ? (To)value : (To)0;
        }
    }
    else if constexpr (sizeof(To) == 4 && (To)-1 > (To)0)
    {
        return value < 0x1p32 && value >= 0.0 ? (To)value : (To)0;
    }
    else
    {
        return (To)(__builtin_fabs(value) < 0x1p31 ? (int)value : (int)(-0x7FFFFFFF - 1));
    }
}
