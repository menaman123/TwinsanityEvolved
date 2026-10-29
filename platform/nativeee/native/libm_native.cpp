// platform/nativeee/native -- compiled INTO the game's native objects on the Vita's native lane only (CMake
// TE_VITA_NATIVE; tools/te_cxx_arm.py like every game TU, then local to the game's prelinked object): the C library
// functions the game calls whose results the wasm module's C library (wasi-libc) defines differently from the Vita's
// newlib. The game's maths calls fmaxf / fminf (math.cpp, the rigid bodies, the hulls, the volumes): wasi-libc returns the
// non-NaN argument for a NaN and, for two zeros, +0 from fmaxf and -0 from fminf (C99 Annex F); newlib's fmaxf(-0, +0)
// returns its first argument. These are wasi-libc's bodies as the wasm module runs them (its FMAX / FMIN after the NaN
// tests), so the native game gets the wasm build's bits.
#include <math.h>

extern "C"
{
    float fmaxf(float x, float y)
    {
        if (x != x)
        {
            return y;
        }
        if (y != y)
        {
            return x;
        }
        if (x == 0.0f && y == 0.0f)
        {
            return signbit(x) ? y : x;
        }
        return x > y ? x : y;
    }

    float fminf(float x, float y)
    {
        if (x != x)
        {
            return y;
        }
        if (y != y)
        {
            return x;
        }
        if (x == 0.0f && y == 0.0f)
        {
            return signbit(x) ? x : y;
        }
        return x < y ? x : y;
    }
}
