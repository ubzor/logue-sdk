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
 *  Keep in sync with the web simulator's formatParamValue (websim/index.html,
 *  the unified drone-chain page)
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
      "RATE", "DET",
      "SPD1", "SM1", "SPD2", "SM2", "SPD3", "SM3",
      "SUB1", "SUB2", "SAT", "ASYM", "FLD", "WA1", "WA2",      "MAIN",
      "TWDR", "DWDR", "FWDR",
      "CUT", "RES", "WFC",
      "BRT", "BRR", "SMT",
      "JIT", "RMOD", "FM1", "FM2", "FMAB", "FMBB",
      "FMVT", "FLVL", "COMB", "SUBB",
      "WLFR", "WLFD",
      "W2LR", "W2LD", "WMIX",
      "WLFS", "WLFG",
      "W2FS", "W2FG",
      "WMXR", "WMXD", "WMXS", "WMXG",
      "FMCR", "FMCW", "FMCD", "FMST", "FMSA", "FMAM", "FCUT2"};
  const bool is_param_sel = (id == PARAM_SEL || id == PARAM_SEL + 2U);
  const bool is_param_val = (id == PARAM_VAL || id == PARAM_VAL + 2U);
  if (is_param_sel)
  {
    return (value >= 0 && value < (int)kEditParamCount) ? names[value] : "";
  }

  // RndM (header slot 4, RND-тип) and Rnd (slot 5, запуск) are type_strings /
  // type_enum; for slot 4 we return the type name ("All"/"Part"/"ORBI") so the
  // device / loguecli / sim show the selector; for slot 5 we return "" and
  // let the platform render the enum value natively (1..15).
  if (id == 4U)
  {
    if (value == kRndModeOrbi)
      return "ORBI";
    return (value == kRndModePart) ? "Part" : "All";
  }
  if (id == 5U)
    return "";
  if (!is_param_val)
    return "";

  // PARAM VAL: format the currently selected parameter's cached raw value.
  const uint8_t idx = (select_ < kEditParamCount) ? select_ : 0;
  const int32_t raw = edit_value_[idx];
  const int32_t k = kEditParamFirst + idx;

  static char buf[8]; // "OFF" / "NNNms" / "N.Ns" / "NNN H" / "NN.Nk" all fit in 8

  // SPDx / SMTx (stream period / time constant): display as 0..100% (the
  // default branch). The actual s / ms value is a log mapping of the raw
  // 0..1023 value; showing % keeps the display consistent and avoids
  // pulling a fasterpowf into the K-rate display path.

  // (INT / INT2 chord intervals were removed — the chord/interval concept is gone.)

  // FM_A / FM_B (phase-FM depth): "Nc" (0..kFmMaxCents, <= 2 digits).
  // Shows the ACTUAL depth after the quadratic kFmCurveExp
  // curve (the audio path applies powf(raw, kFmCurveExp) * kFmMaxCents), so
  // the display never overstates what the knob is doing: low knob -> "0c",
  // full knob -> "0c" (kFmMaxCents = 0.1 < 1 cent, so it reads 0). This is
  // intentional: FM is a subtle timbre knob, not a loud index.
  if (k == FM_A || k == FM_B)
  {
    // kFmCurveExp == 2.0f -> quadratic curve is x^2 (no libm powf: it would
    // show up as an unresolved dynamic symbol on the device).
    const float fx = param_10bit_to_f32(raw);
    int xc = (int)((fx * fx) * kFmMaxCents + 0.5f);
    int n = 0;
    if (xc >= 10) buf[n++] = (char)('0' + xc / 10);
    buf[n++] = (char)('0' + xc % 10);
    buf[n++] = 'c';
    buf[n] = '\0';
    return buf;
  }

  // FMCW (FM Mod Wave): the modulator's waveform name (5 shapes:
  // SIN / TRI / SAW / SQR / NOISE). The same wave drives BOTH the C
  // (vibrato) and S (static teeth) modulators. raw band -> name (mirrors
  // apply_edit_param FM_CW: 0..341 / 342..511 / 512..682 / 683..853 /
  // 854..1023).
  if (k == FM_CW)
  {
    if (raw < 342) return "SIN";
    if (raw < 512) return "TRI";
    if (raw < 683) return "SAW";
    if (raw < 854) return "SQR";
    return "NOISE";
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
  // CUT (SVF cutoff + dry/wet): the cutoff frequency, "NNN H" (< 1 kHz) or
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

  // FMVT (formant vowel position): show the nearest vowel (A/E/I/O/U) as a
  // single letter. The raw 0..1023 value maps to 0..1 across 5 stops, so
  // (int)(pos*4.999) gives the stop index 0..4 (the 4.999 keeps raw=1023
  // from overflowing to index 5).
  if (k == FORM)
  {
    static const char vowels[5] = {'A', 'E', 'I', 'O', 'U'};
    int vi = (int)(param_10bit_to_f32(raw) * 4.999f);
    if (vi < 0) vi = 0;
    else if (vi > 4) vi = 4;
    buf[0] = vowels[vi];
    buf[1] = '\0';
    return buf;
  }

  // WAVE_LFO_RATE (internal wave-LFO frequency): show Hz on the same LOG
  // scale the apply path uses (raw 0 = 0 Hz / OFF; else 10^(3*raw/1023 - 3),
  // 0.001..1 Hz across 3 decades). raw 0 is displayed "0 H" (LFO off); raw
  // 1023 is "1 H" (the max); below 0.1 Hz it is shown as millihertz "0.NNN H"
  // (3 fixed frac digits, e.g. 0.01 Hz -> "0.010 H"); at/above 0.1 Hz it is
  // shown as "0.N H" (1 fixed frac digit, e.g. 0.1 Hz -> "0.1 H"). buf[8] is
  // large enough ("0.999H\0" = 6 chars).
  // WAVE2_LFO_RATE (stream-2 wave-LFO frequency) shares the EXACT same LOG Hz
  // formatting as WAVE_LFO_RATE (stream 1) — the two LFOs use identical scales.
  if (k == WAVE_LFO_RATE || k == WAVE2_LFO_RATE || k == WMIX_LFO_RATE)
  {
    if (raw == 0)
    {
      buf[0] = '0';
      buf[1] = 'H';
      buf[2] = '\0';
      return buf;
    }
    const float hz = fasterpowf(10.f, 3.f * param_10bit_to_f32(raw) - 3.f); // 0.001..1
    if (hz >= 0.1f)
    {
      // 0.1 Hz .. 1 Hz: one fixed frac digit.
      int d = (int)(hz * 10.f + 0.5f); // 1..10
      int n = 0;
      if (d >= 10) // raw 1023 = 1 Hz (the max)
      {
        buf[n++] = '1';
      }
      else
      {
        buf[n++] = '0';
        buf[n++] = '.';
        buf[n++] = (char)('0' + d);
      }
      buf[n++] = 'H';
      buf[n] = '\0';
      return buf;
    }
    // 0.01 Hz .. 0.09 Hz: millihertz, 3 fixed frac digits.
    int mhz = (int)(hz * 1000.f + 0.5f); // 10..99
    if (mhz > 999) mhz = 999;
    if (mhz < 1) mhz = 1;
    int n = 0;
    buf[n++] = '0';
    buf[n++] = '.';
    buf[n++] = (char)('0' + mhz / 100); // "0.N"
    buf[n++] = (char)('0' + (mhz / 10) % 10); // "0.NN"
    buf[n++] = (char)('0' + mhz % 10); // "0.NNN"
    buf[n++] = 'H';
    buf[n] = '\0';
    return buf;
  }

  // ---- LFO shape (WAVE_LFO_SHAPE / WAVE2_LFO_SHAPE) -----------------------
  // Display as "SIN" / "S" / "WALK" (raw 0..511 = SIN, 512..767 = S,
  // 768..1023 = WALK).
  if (k == WAVE_LFO_SHAPE || k == WAVE2_LFO_SHAPE || k == WMIX_LFO_SHAPE)
  {
    if (raw < 512)
      return "SIN";
    if (raw < 768)
      return "S";
    return "WALK";
  }

  // ---- WALK gap (WAVE_LFO_GAP / WAVE2_LFO_GAP) ----------------------------
  // Display as seconds: LOG scale 1..300 s (raw 0..1023).
  // raw 0 -> "1 s", raw 1023 -> "300 s".
  if (k == WAVE_LFO_GAP || k == WAVE2_LFO_GAP || k == WMIX_LFO_GAP)
  {
    const float sec = 1.f * fasterpowf(300.f, param_10bit_to_f32(raw));
    int s = (int)(sec + 0.5f);
    if (s < 1) s = 1;
    if (s > 300) s = 300;
    int n = 0;
    if (s >= 100)
    {
      buf[n++] = (char)('0' + s / 100);
      buf[n++] = (char)('0' + (s / 10) % 10);
      buf[n++] = (char)('0' + s % 10);
    }
    else if (s >= 10)
    {
      buf[n++] = (char)('0' + s / 10);
      buf[n++] = (char)('0' + s % 10);
    }
    else
    {
      buf[n++] = (char)('0' + s);
    }
    buf[n++] = ' ';
    buf[n++] = 's';
    buf[n] = '\0';
    return buf;
  }

  // ---- FM modulator C (FM_CR / FM_CW / FM_CD) -----------------------------
  // FM_CR (rate): LOG 0..1 Hz. raw 0 -> "0 H", else Hz like the wave-LFO rate.
  if (k == FM_CR)
  {
    if (raw == 0)
    {
      buf[0] = '0';
      buf[1] = ' ';
      buf[2] = 'H';
      buf[3] = '\0';
      return buf;
    }
    const float hz = fasterpowf(10.f, 3.f * param_10bit_to_f32(raw) - 3.f) * kFmCRateMaxHz;
    if (hz >= 0.1f)
    {
      const int d = (int)(hz * 10 + 0.5f); // 1..10
      buf[0] = (d >= 10) ? '1' : (char)('0' + d);
      buf[1] = (d >= 10) ? ' ' : '.';
      if (d >= 10) { buf[2] = 'H'; buf[3] = '\0'; }
      else { buf[2] = (char)('0' + d); buf[3] = ' '; buf[4] = 'H'; buf[5] = '\0'; }
      return buf;
    }
    const int mhz = (int)(hz * 1000 + 0.5f); // 1..999
    buf[0] = '0';
    buf[1] = '.';
    int m = mhz < 1 ? 1 : (mhz > 999 ? 999 : mhz);
    buf[2] = (char)('0' + m / 100);
    buf[3] = (char)('0' + (m / 10) % 10);
    buf[4] = (char)('0' + m % 10);
    buf[5] = ' ';
    buf[6] = 'H';
    buf[7] = '\0';
    return buf;
  }
  // FM_CW (wave): SIN / TRI / SAW / SQR / NOISE (band of the raw value, mirrors
  // apply_edit_param FM_CW: 0..341 / 342..511 / 512..682 / 683..853 / 854..1023).
  // NOTE: an EARLIER FMCW branch (right after FM_A/FM_B) already handles this and
  // returns the full names; this is the legacy 4-band branch kept in sync so the
  // two never disagree (the earlier one wins at runtime).
  if (k == FM_CW)
  {
    if (raw < 342) return "SIN";
    if (raw < 512) return "TRI";
    if (raw < 683) return "SAW";
    if (raw < 854) return "SQR";
    return "NS"; // NOISE (the display width is tight; "NS" = noise)
  }
  // FM_CD (depth): 0..100% (LINEAR) -- the default branch formats it.

  // ---- FM modulator S (FM_STATIC / FMST): TEETH per carrier cycle ----------
  // LOG kFmStaticMinTeeth..kFmStaticMaxTeeth (raw 0 = OFF, 0 teeth). Display the
  // tooth count so the user sees "how many teeth per A+B cycle": whole teeth
  // >= 10 as "NN" (10..16), "N." (1..9, one fixed frac digit), "N.NN" (0.05..0.9).
  // raw 0 -> "OFF". The log mapping mirrors apply_edit_param (FM_STATIC).
  if (k == FM_STATIC)
  {
    if (raw == 0) return "OFF";
    const float teeth = kFmStaticMinTeeth *
        fasterpowf(kFmStaticMaxTeeth / kFmStaticMinTeeth, param_10bit_to_f32(raw));
    if (teeth >= 9.5f) // 10..16 -> "NN"
    {
      const int t = (int)(teeth + 0.5f);
      buf[0] = (char)('0' + t / 10);
      buf[1] = (char)('0' + t % 10);
      buf[2] = '\0';
      return buf;
    }
    // < 10 teeth -> "N.F" (1..9) or "0.FF" (0.05..0.99), 1..2 frac digits.
    int n = 0;
    int whole = (int)teeth; // 0..9
    int frac1 = (int)(teeth * 10.f) % 10; // first frac digit
    buf[n++] = (char)('0' + whole);
    buf[n++] = '.';
    buf[n++] = (char)('0' + frac1);
    if (whole < 1) // sub-tooth: show a second frac digit (0.NN)
    {
      const int frac2 = (int)(teeth * 100.f) % 10;
      buf[n++] = (char)('0' + frac2);
    }
    buf[n] = '\0';
    return buf;
  }
  // FMSA (S tooth amplitude): 0..100% (LINEAR) -- the default percent branch
  // below formats it (raw 0 -> "0%", like FM_CD). NOTE: the S tooth SHAPE is
  // shared with the C modulator (FM_CW / FM Mod Wave) and is not a separate
  // display parameter.
  // FMAM (global FM MOD amount): 0..100% (LINEAR) -- the default percent
  // branch below formats it (raw 0 -> "0%", like FM_CD/FMSA). The master
  // multiplier on both the C (vibrato) and S (static teeth) phase offsets;
  // 0% = the whole FM MOD block is off.

  int v;
  if (k == WAV1 || k == WAV2)
  {
    // WAVE (WAV1) / WAVE 2 (WAV2) each show their stream's bank letter (A..F).
    // raw 0..1023 -> bank 0..5 (WAV2 was deprecated when A/B shared one wave;
    // it is now the 2D-morph stream-2 bank, see WAVE_MIX).
    int bank = (int)(param_10bit_to_f32(raw) * 5.999f);
    if (bank < 0) bank = 0;
    if (bank > 5) bank = 5;
    buf[0] = (char)('A' + bank);
    buf[1] = '\0';
    return buf;
  }
  else if (k == WAVE_MIX)
    // WAVE_MIX: symmetric -100..+100% (raw 0..1023 -> -100..+100, 511 = 0).
    // Same signed-percent convention as ASYM: shows the crossfade weight
    // (0% = 50/50, -100% = stream 1 only, +100% = stream 2 only).
    v = (int)((raw * 200) / kRawMax) - 100;
  else if (k == ASYM)
    v = (int)((raw * 200) / kRawMax) - 100;
  else if (k == DETUNE)
    v = (int)((raw - kRawMid) * (int)kDetuneMaxCents / kRawMid); // -kDetuneMaxCents..+kDetuneMaxCents cents (multiply before divide)
  else if (k == SATU)
    v = (int)(param_10bit_to_f32(raw) * (kSatuMax * 100.f)); // 0..300 (per-voice drive)
  else
    v = (int)((raw * 100) / kRawMax);

  // FLD formats as 0..100% (the default branch). SATU shows a bare 0..300 number.
  const bool percent = (k != WAV1 && k != WAV2 && k != SATU && k != DETUNE);
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

