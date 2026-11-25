#pragma once
#include <cstdint>
/*
 *  File: osc_wte.h
 *
 *  Wave-Table-Editor table: 16 morph frames x 256 points, 16-bit.
 *
 *  A FULL COPY of the web /editor mechanism inside the OSC unit: the 16 waves
 *  are NOT a fixed procedural surface (the old ProcTable) but are REGENERATED
 *  from a WTEDoc (16 WaveConfig recipes: sources + combine + modifiers + warp
 *  stacks, see osc_wte_doc.h / osc_wte_render.h) — the same document the web
 *  editor holds. When the UI (the editor panel moved to the chain simulator's
 *  main page) sends a new document (Osc::wteSetDocument, one call, ALL
 *  parameters at once), the 16x256 int16 frame bank is re-rendered at K-rate
 *  in blockPrepass (never in the audio path).
 *
 *  The audio path reads the table from the A+B cluster's OWN per-voice
 *  phasors at a SHARED morph position (see the "WTE wavetable morph" /
 *  "Wavetable samples (WTE)" blocks in osc_mix.h): wte_morph = 7.5 + lfo *
 *  7.5, driven by the internal wave-LFO 1 (the Wave LFO Rate/Depth/Shape
 *  edit params), bilinear crossfade of the two adjacent frames + two
 *  adjacent phase points. The table IS the main voice: its level is MAIN
 *  and the old PMORPH/PLEV edit params are no-ops (superseded).
 *
 *  MEMORY
 *  ------
 *  frames_[16][256] int16 = 8 KB (BSS) + the document cache (WTEDoc, ~12 KB:
 *  16 waves x 4 sources x (4 warps x 9 params) ... see the struct). No dynamic
 *  allocation (getBufferSize() == 0, like the rest of the unit).
 *
 *  NO libm in the audio path: the lookup is plain shifts/masks + FMAs.
 *  Generation is K-rate-only and uses the fast helpers from the renderer.
 */

#include "osc_wte_doc.h"
#include "osc_wte_render.h"

struct WteTable
{
  // ---- Dimensions (constant) ------------------------------------------------
  static constexpr uint32_t kFrames = WTE_WAVE_COUNT; // 16 morph frames
  static constexpr uint32_t kPoints = 256U;           // points per frame (256 = 1<<8)
  static constexpr uint32_t kMask   = kPoints - 1U;

  // ---- Frame data (int16, 8 KB) ---------------------------------------------
  int16_t frames_[kFrames][kPoints];

  // The document this frame bank was generated from (cached so regen() is a
  // pure function of doc_; set by set_doc() which also raises `dirty`).
  WTEDoc doc_;

  // ---- Generation state -------------------------------------------------------
  bool  dirty = true;    // regenerate on next blockPrepass
  bool  active = false;  // web-sim flag: the table drives the voice (set together with set_doc)

  // ---- Import (called from the web-sim channel, main thread) ------------------
  // Store the whole document (ALL 16 waves, ALL their parameters) and mark the
  // table dirty. This is the "задать можно только сразу все" entry point: the
  // UI sends the complete document in one call. K-rate regen happens later in
  // blockPrepass (audio thread) — set_doc() itself does no DSP.
  void set_doc(const WTEDoc &d)
  {
    doc_ = d;
    dirty = true;
    active = true;
  }

  // Re-render all 16 frames from doc_ (K-rate, called from Osc::wteRegen()
  // when `dirty` is set). 16 x 256 x (up to 4 sources x up to ~9 warp stages)
  // — a few million flops, sub-millisecond on the STM32H725 hard FPU, trivial
  // in the web sim.
  void regen()
  {
    float row[kPoints];
    for (uint32_t f = 0; f < kFrames; f++)
    {
      wte::render_wave(doc_.waves[f], row, kPoints, f);
      int16_t *fr = frames_[f];
      for (uint32_t i = 0; i < kPoints; i++)
      {
        int32_t v = (int32_t)(row[i] * 32767.f);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        fr[i] = (int16_t)v;
      }
    }
    dirty = false;
  }

  // ---- Audio-path lookup ------------------------------------------------------
  // Resolve the wavetable sample at phase ph (0..1) for a fractional morph
  // position pos (0..kFrames-1 = 0..15). Bilinear: crossfade the two adjacent
  // frames (by frac of pos), each frame linearly interpolated in phase.
  // Returns [-1, +1] (int16 / 32768). Pure shifts/masks + FMAs — no libm,
  // safe in the per-sample audio path. Same algorithm as ProcTable::lookup.
  static float lookup(const int16_t (&frames)[kFrames][kPoints], float pos, float ph)
  {
    if (pos < 0.f) pos = 0.f;
    const float maxpos = static_cast<float>(kFrames - 1);
    if (pos > maxpos) pos = maxpos;
    const uint32_t i = (uint32_t)pos;
    const float frac = pos - static_cast<float>(i);

    float pf = ph * static_cast<float>(kPoints);
    if (pf < 0.f) pf = 0.f;
    else if (pf >= static_cast<float>(kPoints)) pf -= static_cast<float>(kPoints);
    const uint32_t pi = (uint32_t)pf;
    const float pfr = pf - static_cast<float>(pi);
    const uint32_t pi1 = (pi + 1U) & kMask;

    const int16_t *fa = frames[i];
    const float sa = static_cast<float>(fa[pi]) * (1.f - pfr) +
                     static_cast<float>(fa[pi1]) * pfr;

    float result = sa;
    if (frac > 0.001f && i + 1U < kFrames)
    {
      const int16_t *fb = frames[i + 1U];
      const float sb = static_cast<float>(fb[pi]) * (1.f - pfr) +
                       static_cast<float>(fb[pi1]) * pfr;
      result = sa * (1.f - frac) + sb * frac;
    }
    return result * (1.f / 32768.f);
  }
};