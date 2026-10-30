#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_lfo_sim.h
 *
 *  Simulator-only shape-LFO synthesizer for the DRONE unit.
 *
 *  On hardware the platform generates the shape-LFO from the OSC-button A/B
 *  knobs and delivers it to the Osc via setShapeLfo() (K-rate). In the web
 *  simulator there is no platform, so the LFO is synthesized at audio rate
 *  here as a phase-coherent sine. The UI (websim/osc.html) sets the frequency
 *  (F knob, 0..30 Hz) and depth (S knob, 0..1) via setLfoFreq / setLfoDepth.
 *
 *  This file is only meaningful under EMSCRIPTEN (the simulator build);
 *  on ARM the Osc receives lfo_ from the platform and this struct is not
 *  instantiated. The Osc (osc.h) owns a ShapeLfoSim member under
 *  #ifdef EMSCRIPTEN and calls process() per sample in the audio path.
 *
 */

#include "utils/float_math.h" // osc_sinf()

struct ShapeLfoSim
{
  // Two pi (phase increment helper).
  static constexpr float kTwoPi = 6.283185307179586f;

  // Audio sample rate (constant across all logue products).
  static constexpr float kSampleRate = 48000.f;

  // ---- State (per-sample, owned by the Osc) ----

  float phase  = 0.f;  // LFO phase (0..1)
  float freq   = 1.f;  // LFO frequency in Hz (set by the UI, 0..30 Hz)
  float depth  = 0.f;  // LFO depth (0..1, set by the UI; 0 = LFO off)

  // ---- Audio path (called once per sample from Osc::process) ----

  // Advance the LFO by one sample and return the LFO value in (-1, 1).
  // If depth is 0, the LFO is off and 0 is returned.
  float process()
  {
    if (depth > 0.f)
    {
      phase += kTwoPi * freq * (1.f / kSampleRate);
      if (phase >= 1.f)
        phase -= 1.f;
      return osc_sinf(phase) * depth;
    }
    return 0.f;
  }

  // Reset the LFO state (phase to 0, depth to 0 = off).
  void reset()
  {
    phase = 0.f;
    depth = 0.f;
  }
};
