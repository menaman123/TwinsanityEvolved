#include "games/slus_20909_crash_twinsanity/hle/math/joint_math.h"

#include <cfenv>
#include <cmath>
#include <cstring>

#include "engine/vu/vu_float.h"

// The definitions are joint_math_bodies.inl's: out of line here, in line through joint_math.h on the Vita
#if !defined(__vita__)
#define TE_JM_DEF
#include "games/slus_20909_crash_twinsanity/hle/math/joint_math_bodies.inl"
#endif
