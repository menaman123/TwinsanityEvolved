// te_math_diag.h -- diagnosis of the VU0 matrix seam (vu0_matrices.cpp): VuTransformPoint and VuRotateVector against the
// retail functions' own instructions on the engine's VU interpreter. Development only; nothing here runs unless asked.
//
//   TE_MATH_TRANSFORM_COMPARE=1  every call is also answered by (a) the engine's HLE replacement the recompiled reference
//                                runs (title::MathLibrary::EvaluateTransformPoint / EvaluateRotation) and (b) the retail
//                                function's instructions (MultiplyMatrixByVector 0x18EC80 / TransformVector_ 0x18EC48 of the
//                                PAL executable, read from the disc at boot: lqc2, the COP2 FMAC words, sqc2), each FMAC word
//                                executed as a VU upper instruction on VuInterpreterTier (the VU's operand clamps, round
//                                toward zero, the MADD family fused); the three answers are compared bit for bit and the counts
//                                printed at exit ("[te.math] transform compared: ..."). The port's own answer is what the game
//                                gets.
//   TE_MATH_TRANSFORM_MICRO=1    the interpreter's answer is the one the game gets (the A/B of the arithmetic; implies the
//                                comparison's counts).
//   TE_MATH_TRANSFORM_WATCH=0xADDR[,0xADDR...]   matrices (a rigid body's matrix: its body + 0xB0) whose calls are counted
//                                apart: the spring corners' and the hull's heights of that body ("watched" in the report).
//   TE_MATH_TRANSFORM_BIAS_Y=D   D added to the y of every VuTransformPoint answer on a watched matrix: a perturbation of the
//                                seam for one body (its level springs read their corner heights through it, so the body settles
//                                ~D lower) -- the iceclimb_bergs judge's sensitivity check (RED by heights, the route intact).
#pragma once
#include <cstddef>
#include <cstdint>

namespace te::math {

// Whether either knob is set (the runner then reads the retail words out of the executable it loaded).
bool MacroDiagRequested();

// The retail functions' words, from their first instruction (the decoder stops after `jr ra` and its delay slot).
void SetRetailMacroWords(const uint32_t* transform_point, size_t transform_point_words, const uint32_t* rotate_vector,
                         size_t rotate_vector_words);

}  // namespace te::math
