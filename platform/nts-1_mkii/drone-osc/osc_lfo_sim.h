#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_lfo_sim.h
 *
 *  Internal wave-LFO synthesizer for the DRONE unit.
 *
 *  The wavetable intra-bank morph (see osc_waves.h / osc_mix.h) is driven by
 *  THIS internal LFO, not the platform shape-LFO. The platform's OSC-button
 *  RATE/SHAPE knobs are now NO-OPs (setShapeLfo() is a stub) -- they were
 *  too coarse (0.1 Hz step) and were bound to the platform, so the LFO is
 *  fully INTERNAL: its frequency (freq_hz_) and depth (morph_amt_) are set by
 *  the two editable parameters WAVE_LFO_RATE / WAVE_LFO_DEPTH through the
 *  standard apply_edit_param() path (osc_param_apply.h), so both the hardware
 *  PARAM/VALUE multiplexer and the web-sim's per-parameter sliders drive it.
 *  The Osc owns ONE ShapeLfoSim and calls process() per sample in the audio
 *  path (osc_mix.h, under #ifdef EMSCRIPTEN for the web-sim synthesis; on
 *  ARM the same member is used, since the LFO is now internal and the
 *  platform shape-LFO no longer feeds the audio path).
 *
 *  process() returns the LFO value in [-1, +1] (the sine value scaled by the
 *  depth); the caller multiplies it by the WaveTable::lfo_morph_depth(bank)
 *  and adds it to the bank center to get the intra-bank morph position.
 *
 */

#include "utils/float_math.h" // osc_sinf()

struct ShapeLfoSim
{
  // Audio sample rate (constant across all logue products).
  static constexpr float kSampleRate = 48000.f;

  // ---- State (per-sample, owned by the Osc) ----

  float phase = 0.f;   // LFO phase in CYCLES (0..1)
  // LFO frequency in Hz (set by the WAVE_LFO_RATE edit parameter; the apply
  // path maps raw 0 -> 0 Hz (LFO OFF), else raw 1..1023 -> 0.001..1 Hz on a
  // LOG scale (10^(3*raw/1023 - 3), 3 decades), finer near 0). The factory
  // default is 0 Hz (LFO OFF -- static wave); turning the knob up drives a
  // slow, organic morph. 0 = LFO off (phase does not advance, output is 0).
  float freq_hz = 0.f;
  // LFO morph depth (set by the WAVE_LFO_DEPTH edit parameter; raw 0..1023 ->
  // 0..1 LINEAR; the factory default is 1.0 = a full-bank sweep). At 0 the
  // LFO is off (process() returns 0, the wave stays at the bank center). At 1
  // the morph sweeps from the first to the last table of the bank (and back).
  float morph_amt = 0.f;

  // ---- Audio path (called once per sample from Osc::process) ----

  // Advance the LFO by one sample and return the LFO value in [-1, +1]
  // (sine * depth). If morph_amt is 0, the LFO is off and 0 is returned
  // (the phase still advances if freq_hz > 0, so re-enabling depth does not
  // jump the waveform -- a minor nicety; at freq_hz = 0 the phase is frozen).
  //
  // Phase convention: phase is in CYCLES (0..1). We advance it by
  // freq_hz * (1/sampleRate) cycles per sample, and osc_sinf(phase) computes
  // sin(2*pi*phase) — the 2*pi lives INSIDE osc_sinf. Advancing by
  // 2*pi*freq/sr instead (the old code) double-applied 2*pi: setting
  // freq = 0.1 Hz produced a 0.628 Hz LFO (~1.6 s period) and a broken
  // (truncated) waveform, because the `phase >= 1.f` wrap then triggered
  // in the middle of the sine.
  float process()
  {
    if (freq_hz > 0.f)
    {
      phase += freq_hz * (1.f / kSampleRate);
      if (phase >= 1.f)
        phase -= 1.f;
    }
    if (morph_amt > 0.f)
      return osc_sinf(phase) * morph_amt;
    return 0.f;
  }

  // Reset the LFO state (phase to 0; freq/depth are NOT touched -- they are
  // owned by the edit parameters and re-applied by apply_edit_param).
  void reset()
  {
    phase = 0.f;
  }
};
