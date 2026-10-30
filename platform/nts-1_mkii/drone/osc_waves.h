#pragma once
/*
    BSD 3-Clause License

    Copyright (c) 2023, KORG INC.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this
      list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

    * Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
    OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

//*/

/*
 *  File: osc_waves.h
 *
 *  Wavetable scanning & pitch-shifted morph for the DRONE unit.
 *
 *  Each of the two oscillators scans one of the 91 Korg wavetables (six banks
 *  A-F, airy -> dense) selected by its WAVE knob. A continuous morph position
 *  is mapped onto the bank arrays (resolve_wave), and the morph point drifts
 *  slightly toward the denser banks as the pitch rises so the tone stays
 *  consistent across the keyboard.
 *
 *  This is a self-contained, stateless-ish helper: it owns only the cached
 *  pitch-shifted morph position (wave_shift_). Osc (osc.h) declares it a
 *  friend so the audio path can read wave_shift_ directly without a getter
 *  indirection.
 *
 *  The bank sizes (k_waves_a_cnt .. k_waves_f_cnt) and the wavesA..F /
 *  osc_wave_scanf() LUT live in unit_osc.h (provided by the NTS-1 firmware on
 *  hardware, linked from the websim stubs in the browser).
 *
 */

#include "unit_osc.h"

struct WaveTable
{
  // Total wavetable count across all six Korg banks (A-F), in order of
  // increasing harmonic density. The WAV1 / WAV2 knobs select an index
  // in [0, kWavesTotal) which resolve_wave() maps onto the bank arrays.
  static constexpr uint32_t kWavesA = k_waves_a_cnt;
  static constexpr uint32_t kWavesB = kWavesA + k_waves_b_cnt;
  static constexpr uint32_t kWavesC = kWavesB + k_waves_c_cnt;
  static constexpr uint32_t kWavesD = kWavesC + k_waves_d_cnt;
  static constexpr uint32_t kWavesE = kWavesD + k_waves_e_cnt;
  static constexpr uint32_t kWavesTotal = kWavesE + k_waves_f_cnt;

  // LFO -> wavetable morph depth (+/-N tables). The platform shape-LFO (the
  // "S" branch of the OSC B-knob on hardware) arrives at the Osc as lfo_ in
  // (-1, 1); the Osc scales it so full depth sweeps +/-10 of the 91 tables
  // (~11% of the range) — a moderate "breathing" of the timbre across the
  // wavetable. The morph is applied continuously inside the sample loop (see
  // Osc::process()), blended with the pitch-shifted morph position, and
  // resolved through the interpolating resolve_wave() so the timbre glides
  // between adjacent tables.
  static constexpr float kLfoMorphDepth = 10.f;

  // Pitch-shifted morph position (in table-index units). Higher notes morph
  // toward the denser F bank so the perceived tone stays consistent across the
  // keyboard, mirroring how the SDK's band-limited waves roll off their high
  // harmonics. kWaveMorphShift is the max shift (in tables) at the top of the
  // range. Recomputed on setParameter(WAV1/WAV2) and setPitch() (K-rate) and
  // cached so the audio path only adds the LFO offset (see Osc::process()).
  static constexpr float kWaveMorphShift = 10.f; // ~1/9 of the bank range

  // Cached pitch-shifted morph position (table-index units), recomputed by
  // update() from the base (saw) frequency. Read by the Osc audio path.
  float wave_shift_ = 0.f;

  // ---- Sample-rate / reference-frequency helpers (constant across all
  // logue products; the Osc keeps its own kSampleRate/kRefHz copies for its
  // non-wavetable sites) ----
  static constexpr float kSampleRate = 48000.f; // 48 kHz
  static constexpr float kRefHz = 880.f;        // A4

  // Map a fractional wavetable index in [0, kWavesTotal) to a blended table
  // sample at phase ph. The six Korg banks (A-F) live in separate arrays; this
  // walks the bank thresholds to find the table and the fractional part
  // linearly crossfades between that table and the next one (or the previous
  // one, to avoid a wrap-around blend across the F->A boundary). Blending two
  // adjacent tables is what makes the LFO morph glide smoothly instead of
  // stepping table-to-table. Falls back to a single-table scan when the index
  // is effectively integral or at the top edge (no next table to blend).
  static float resolve_wave(float idx, float ph)
  {
    float lo = idx < 0.f ? 0.f : idx;
    if (lo >= static_cast<float>(kWavesTotal - 1))
      lo = static_cast<float>(kWavesTotal - 1);
    const uint32_t i = static_cast<uint32_t>(lo);
    const float frac = lo - static_cast<float>(i);

    const float *ta =
        (i < kWavesA) ? wavesA[i] :
        (i < kWavesB) ? wavesB[i - kWavesA] :
        (i < kWavesC) ? wavesC[i - kWavesB] :
        (i < kWavesD) ? wavesD[i - kWavesC] :
        (i < kWavesE) ? wavesE[i - kWavesD] :
                        wavesF[i - kWavesE];

    if (frac < 0.001f)
      return osc_wave_scanf(ta, ph);

    // next table index in the global 0..kWavesTotal-1 space
    const uint32_t j = i + 1U;
    const float *tb =
        (j < kWavesA) ? wavesA[j] :
        (j < kWavesB) ? wavesB[j - kWavesA] :
        (j < kWavesC) ? wavesC[j - kWavesB] :
        (j < kWavesD) ? wavesD[j - kWavesC] :
        (j < kWavesE) ? wavesE[j - kWavesD] :
                        wavesF[j - kWavesE];

    return osc_wave_scanf(ta, ph) * (1.f - frac) + osc_wave_scanf(tb, ph) * frac;
  }

  // Recompute the pitch-shifted morph position from the base (saw) phase
  // increment w0 (w0 = f / sample_rate, so w0 * kSampleRate is the base Hz).
  // Called from the Osc on setPitch() and setParameter(WAV1/WAV2) — K-rate,
  // never in the audio path.
  void update(float w0)
  {
    const float pitch_n = (w0 * kSampleRate) / kRefHz; // 1.0 at A4
    const float shift = (pitch_n < 0.f) ? 0.f : (pitch_n > 1.f) ? kWaveMorphShift : (kWaveMorphShift * pitch_n);
    wave_shift_ = shift;
  }
};
