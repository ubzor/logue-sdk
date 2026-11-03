#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_param_display.h
 *
 *  Out-of-line definition of Osc::getParameterStrValue() -- the formatter that
 *  turns a header-slot id (0..3) into the display string shown by the unit
 *  (unit_get_param_str_value -> on-device display and the web sim).
 *
 *  This was the second-largest block of osc.h (a ~145-line formatter). It now
 *  lives here so the class body stays small. It is #included at the BOTTOM of
 *  osc.h, after the Osc class is complete, so it can reference select_ /
 *  edit_value_, the OscParams enum, and the osc_consts::* scaling constants.
 *
 *  Keep in sync with the web simulator's formatParamValue (websim/osc.html)
 *  and apply_edit_param (osc_param_apply.h): all three share the same
 *  raw <-> DSP ranges, so the display, the DSP, and the UI always agree.
 *
 */

#include "osc.h"  // Osc class (complete definition) + osc_consts::* + helpers

inline const char *Osc::getParameterStrValue(uint8_t id, int32_t value) const
{
  // Note: String memory must be accessible even after function returned.
  //       It can be assumed that caller will have copied or used the string
  //       before the next call to getParameterStrValue.
  static const char *const names[kEditParamCount] = {
      "DETU", "RATE",
      "SPD1", "SMT1", "SPD2", "SMT2", "SPD3", "SMT3",
      "SUB1", "SUB2", "SATU", "ASYM", "FOLD", "WAV1", "WAV2", "WAV3",
      "CHRD", "CINT",
      "MAIN",
      "RWAV", "RDRV", "RFLD",
      "XFM1", "XFM2", "FCUT", "RESO", "RWFC",
      "RVLL", "RVLS", "RVLS",
      "SATC", "FLDC"};
  const bool is_param_sel = (id == PARAM_SEL || id == PARAM_SEL + 2U);
  const bool is_param_val = (id == PARAM_VAL || id == PARAM_VAL + 2U);
  if (is_param_sel)
  {
    return (value >= 0 && value < (int)kEditParamCount) ? names[value] : "";
  }
  if (!is_param_val)
    return "";

  // PARAM VAL: format the currently selected parameter's cached raw value.
  const uint8_t idx = (select_ < kEditParamCount) ? select_ : 0;
  const int32_t raw = edit_value_[idx];
  const int32_t k = kEditParamFirst + idx;

  static char buf[8];

  // SPDx / SMTx (stream period / time constant): display as 0..100% (the
  // default branch). The actual s / ms value is a log mapping of the raw
  // 0..1023 value; showing % keeps the display consistent and avoids
  // pulling a fasterpowf into the K-rate display path.

  // CINT (chord interval): display cents (0..1900), not a name.
  if (k == CHRD_INT)
  {
    int ci = (int)(param_10bit_to_f32(raw) * ChordVoice::kMaxCents + 0.5f);
    // format as "NNNc" (e.g. "700c", "1900c")
    int n2 = 0;
    int t2 = ci;
    char d2[5];
    int nd2 = 0;
    do
    {
      d2[nd2++] = (char)('0' + t2 % 10);
      t2 /= 10;
    } while (t2 > 0);
    for (int i = nd2 - 1; i >= 0; --i)
      buf[n2++] = d2[i];
    buf[n2++] = 'c';
    buf[n2] = '\0';
    return buf;
  }

  // XFM_A / XFM_B (cross-FM depth): "Nc" (0..kXfmMaxCents, <= 2 digits).
  // No printf in the image (the codebase hand-rolls formatting), so write the
  // at-most-2-digit value directly.
  if (k == XFM_A || k == XFM_B)
  {
    int xc = (int)(param_10bit_to_f32(raw) * kXfmMaxCents + 0.5f);
    int n = 0;
    if (xc >= 10) buf[n++] = (char)('0' + xc / 10);
    buf[n++] = (char)('0' + xc % 10);
    buf[n++] = 'c';
    buf[n] = '\0';
    return buf;
  }

  // RES (SVF resonance Q): "X.Y" (0.5..2.5, fixed 3 chars, no loop needed).
  if (k == RES)
  {
    int q10 = (int)(Svf::quality(param_10bit_to_f32(raw)) * 10.f + 0.5f); // 5..25
    buf[0] = (char)('0' + q10 / 10);
    buf[1] = '.';
    buf[2] = (char)('0' + q10 % 10);
    buf[3] = '\0';
    return buf;
  }
  // FCUT (SVF cutoff + dry/wet): the cutoff frequency, "NNN H" (< 1 kHz) or
  // "NN.Nk" (>= 1 kHz, at most 2 whole + 1 frac digit since max is 12.0k).
  if (k == FCUT)
  {
    int hz = (int)(Svf::cutoffHz(param_10bit_to_f32(raw)) + 0.5f); // 80..12000
    int n = 0;
    if (hz >= 1000)
    {
      // "NN.Nk"
      int k10 = hz / 100; // e.g. 9600 -> 96 (N.N * 10), 12000 -> 120
      int whole = k10 / 10; // 9 / 12
      int frac = k10 % 10;  // 6 / 0
      if (whole >= 10) buf[n++] = (char)('0' + whole / 10);
      buf[n++] = (char)('0' + whole % 10);
      buf[n++] = '.';
      buf[n++] = (char)('0' + frac);
      buf[n++] = 'k';
    }
    else
    {
      // "NNN H" (80..999)
      int t = hz;
      if (t >= 100) buf[n++] = (char)('0' + t / 100);
      t %= 100;
      if (t >= 10 || hz >= 100) buf[n++] = (char)('0' + t / 10);
      buf[n++] = (char)('0' + t % 10);
      buf[n++] = 'H';
    }
    buf[n] = '\0';
    return buf;
  }

  int v;
  if (k == WAV1 || k == WAV2 || k == WAV3)
    v = (int)(param_10bit_to_f32(raw) * (WaveTable::kWavesTotal - 1));
  else if (k == ASYM)
    v = (int)((raw * 200) / kRawMax) - 100;
  else if (k == DETUNE)
    v = (int)((raw - kRawMid) * (int)kDetuneMaxCents / kRawMid); // -kDetuneMaxCents..+kDetuneMaxCents cents (multiply before divide)
  else if (k == SATU || k == CHSAT)
    v = (int)(param_10bit_to_f32(raw) * (kSatuMax * 100.f)); // 0..300 (CHSAT = chord's own drive)
  else
    v = (int)((raw * 100) / kRawMax);

  // CHFLD formats as 0..100% (the default branch), so it is NOT in this
  // no-percent list. CHSAT, like SATU, shows a bare 0..300 number.
  const bool percent = (k != WAV1 && k != WAV2 && k != WAV3 && k != SATU && k != CHSAT && k != DETUNE && k != CHRD_INT);
  int n = 0;
  if (v < 0)
    buf[n++] = '-';
  else if (k == ASYM)
    buf[n++] = '+';
  int t = (v < 0) ? -v : v;
  char d[4];
  int nd = 0;
  do
  {
    d[nd++] = (char)('0' + t % 10);
    t /= 10;
  } while (t > 0);
  for (int i = nd - 1; i >= 0; --i)
    buf[n++] = d[i];
  if (percent)
    buf[n++] = '%';
  buf[n] = '\0';
  return buf;
}

