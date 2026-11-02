// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once
/*
 * A stand-in for the microKORG2 SDK's unit_osc.h, for this host test only.
 *
 * The real header pulls in utils/float_simd.h and utils/int_simd.h, which use
 * NEON types (int2x2_t and friends) unconditionally in places and so do not
 * compile on anything but ARM.  Everything this test drives -- the parameter
 * table, the voice walk, the note and gate dispatch, the render scatter -- is
 * on the unit's side of the boundary, so the test stands the header in rather
 * than the unit's own source being bent to suit a host compiler.
 *
 * The oscillator context is copied field for field from the real one, and
 * GetBufferOffset() is copied from it verbatim: the unit calls the SDK's own
 * helper rather than open-coding the arithmetic, so the stand-in has to carry
 * it.  It is four lines and it is the thing under test, so it is copied rather
 * than reimplemented.  unit.h and runtime.h come from the SDK as they are.
 */

#include <stdint.h>

#include "runtime.h"

typedef struct unit_runtime_osc_context {
  float pitch[kMk2MaxVoices];
  uint8_t trigger;  /* bit array */
  float * unitModDataPlus;
  float * unitModDataPlusMinus;
  uint8_t modDataSize;
  uint16_t bufferOffset;
  uint8_t voiceOffset;
  uint8_t voiceLimit;
  uint16_t outputStride;
} unit_runtime_osc_context_t;

/* Copied from the SDK's unit_osc.h. */
static inline uint32_t GetBufferOffset(const unit_runtime_osc_context_t * context,
                                       int voice, int32_t frameSize) {
  return context->bufferOffset + (voice >> 2) * (frameSize << 2);
}
