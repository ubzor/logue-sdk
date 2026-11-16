#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_waves.h
 *
 *  Wavetable bank selection & intra-bank morph for the DRONE unit.
 *
 *  The WAVE knob selects a BANK (A..F, 0..5). The LFO then drives a
 *  CONTINUOUS position WITHIN THE SELECTED BANK, centered on the bank: at
 *  LFO=0 the position sits at the bank center, and at LFO=+/-100% it sweeps
 *  from the first to the last table of the bank (and back), covering every
 *  wave in the bank. The audio path resolves the current wave as a linear
 *  blend of the two adjacent tables in the bank (osc_bl2_parf-style: sample
 *  table floor(pos) and table ceil(pos) at the same phase, crossfade by the
 *  fractional part). The bank boundary is hard: the position clamps to
 *  [0, bank_size-1] and never crosses into the next bank.
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
  // increasing harmonic density. The WAVE knob selects a bank (0..5); the
  // LFO drives the intra-bank morph position centered on the bank.
  static constexpr uint32_t kWavesA = k_waves_a_cnt;
  static constexpr uint32_t kWavesB = kWavesA + k_waves_b_cnt;
  static constexpr uint32_t kWavesC = kWavesB + k_waves_c_cnt;
  static constexpr uint32_t kWavesD = kWavesC + k_waves_d_cnt;
  static constexpr uint32_t kWavesE = kWavesD + k_waves_e_cnt;
  static constexpr uint32_t kWavesTotal = kWavesE + k_waves_f_cnt;

  // ---- Bank access helpers --------------------------------------------------

  // Map a GLOBAL wave index (0..kWavesTotal-1) to its BANK index (0..5).
  static uint32_t bank_index(uint32_t global_idx)
  {
    if (global_idx >= kWavesTotal) global_idx = kWavesTotal - 1U;
    if (global_idx < kWavesA) return 0U;
    if (global_idx < kWavesB) return 1U;
    if (global_idx < kWavesC) return 2U;
    if (global_idx < kWavesD) return 3U;
    if (global_idx < kWavesE) return 4U;
    return 5U;
  }

  // Bank size (number of tables) for a given bank index (0..5).
  static uint32_t bank_size(uint32_t bank)
  {
    switch (bank)
    {
      case 0U: return k_waves_a_cnt;
      case 1U: return k_waves_b_cnt;
      case 2U: return k_waves_c_cnt;
      case 3U: return k_waves_d_cnt;
      case 4U: return k_waves_e_cnt;
      default: return k_waves_f_cnt;
    }
  }

  // Starting global index of a given bank (0..5).
  static uint32_t bank_start(uint32_t bank)
  {
    switch (bank)
    {
      case 0U: return 0U;
      case 1U: return kWavesA;
      case 2U: return kWavesB;
      case 3U: return kWavesC;
      case 4U: return kWavesD;
      default: return kWavesE;
    }
  }

  // Resolve a GLOBAL wave index to a pointer to the table data.
  static const float *table_ptr(uint32_t global_idx)
  {
    if (global_idx >= kWavesTotal) global_idx = kWavesTotal - 1U;
    if (global_idx < kWavesA) return wavesA[global_idx];
    if (global_idx < kWavesB) return wavesB[global_idx - kWavesA];
    if (global_idx < kWavesC) return wavesC[global_idx - kWavesB];
    if (global_idx < kWavesD) return wavesD[global_idx - kWavesC];
    if (global_idx < kWavesE) return wavesE[global_idx - kWavesD];
    return wavesF[global_idx - kWavesE];
  }

  // ---- LFO morph depth ------------------------------------------------------
  // LFO -> intra-bank wavetable morph depth, in TABLES (fractional bank
  // positions). The platform shape-LFO arrives at the Osc as lfo_ in (-1, 1);
  // the Osc scales it so full depth sweeps +/-half of the (size-1) span.
  // The morph is CENTERED: at lfo_=0 the position sits at the bank center,
  // and at lfo_ = +/-1 it sweeps from the first to the last table of the
  // bank (and back), covering every wave in the bank. The position clamps
  // to [0, bank_size-1], so the LFO can never push the morph past the bank
  // boundary.
  static constexpr float kLfoMorphDepthHalfSpan = 0.5f; // fraction of (size-1) span

  // Bank center position (fractional table index), where lfo_=0 lands.
  static float bank_center(uint32_t bank)
  {
    return (static_cast<float>(bank_size(bank)) - 1.f) * 0.5f;
  }

  // Compute the LFO morph depth (in tables) for a given bank: at lfo_ = +/-1
  // the position moves this far from the center, reaching the bank edges.
  static float lfo_morph_depth(uint32_t bank)
  {
    return (static_cast<float>(bank_size(bank)) - 1.f) * kLfoMorphDepthHalfSpan;
  }

  // ---- Sample-rate / reference-frequency helpers (constant across all
  // logue products; the Osc keeps its own kSampleRate/kRefHz copies for its
  // non-wavetable sites) ----
  static constexpr float kSampleRate = 48000.f; // 48 kHz
  static constexpr float kRefHz = 880.f;        // A4

  // ---- Intra-bank wave resolution -------------------------------------------
  // Resolve the current wave sample at phase ph, given:
  //   bank    — the selected bank (0..5, A..F)
  //   pos     — the ABSOLUTE position within the bank in tables (fractional,
  //             0..bank_size-1). The caller adds the LFO offset to the bank
  //             center (see bank_center / lfo_morph_depth).
  //
  // pos clamps to [0, bank_size-1] (hard bank boundary). The fractional part
  // of pos crossfades between the two adjacent tables in the bank
  // (osc_bl2_parf-style: both tables sampled at the same phase, blended by
  // the fractional part). If pos is effectively integral, a single-table
  // scan is used (no crossfade cost).
  static float resolve_wave(uint32_t bank, float pos, float ph)
  {
    const uint32_t bstart = bank_start(bank);
    const float bank_sz = static_cast<float>(bank_size(bank));

    // Position within the bank: 0..(bank_size-1)
    if (pos < 0.f) pos = 0.f;
    if (pos > bank_sz - 1.f) pos = bank_sz - 1.f;

    const uint32_t i = static_cast<uint32_t>(pos);
    const float frac = pos - static_cast<float>(i);

    const float *ta = table_ptr(bstart + i);
    if (frac < 0.001f)
      return osc_wave_scanf(ta, ph);

    // Next table within the bank (i+1 is always valid: pos < bank_size-1
    // when frac > 0.001, because we clamped pos to bank_size-1).
    const float *tb = table_ptr(bstart + i + 1U);

    return osc_wave_scanf(ta, ph) * (1.f - frac) + osc_wave_scanf(tb, ph) * frac;
  }
};
