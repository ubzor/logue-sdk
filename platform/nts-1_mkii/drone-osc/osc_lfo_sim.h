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
 *  THIS internal LFO, not the platform shape-LFO. The LFO is fully INTERNAL:
 *  its frequency (freq_hz_) and depth (morph_amt_) are set by the WAVE_LFO_RATE
 *  / WAVE_LFO_DEPTH edit parameters through the standard apply_edit_param()
 *  path (osc_param_apply.h). The Osc owns THREE ShapeLfoSim instances:
 *  lfo_sim_ for stream 1, lfo_sim2_ for stream 2, and lfo_mix_ for the
 *  WAVE-MIX balance (it modulates the stream 1 <-> stream 2 crossfade weight
 *  around the static WAVE_MIX value; see the "wave-mix balance" block in
 *  osc_mix.h), and calls process() on each per sample.
 *
 *  process() returns the LFO value in [-1, +1] (shape * depth); the caller
 *  multiplies it by the WaveTable::lfo_morph_depth(bank) and adds it to the
 *  bank center to get the intra-bank morph position.
 *
 *  ---- LFO SHAPE (three modes, per-stream) ----------------------------------
 *  0. SIN  — classic sine (default, bit-identical to the old code).
 *  1. S    — S-curve: asymmetric trajectory with a local reversal.
 *  2. WALK — random-walk scene: every LFO cycle a NEW random trajectory is
 *            generated (control points with random gaps, Catmull-Rom smoothed).
 *            Single tunable: WAVE_LFO_GAP (mean seconds between points,
 *            LOG 1..300 s, default 30 s).
 *
 */

#include "utils/float_math.h" // osc_sinf(), fasterlogf()

struct ShapeLfoSim
{
  static constexpr float kSampleRate = 48000.f;

  // ---- LFO shape modes ----
  static constexpr uint8_t kShapeSin  = 0;
  static constexpr uint8_t kShapeS    = 1;
  static constexpr uint8_t kShapeWalk = 2;

  // Max control points in a WALK scene (safety cap).
  static constexpr uint8_t kWalkMaxPoints = 64;

  // ---- State (per-sample, owned by the Osc) ----
  float phase = 0.f;   // LFO phase in CYCLES (0..1)
  float freq_hz = 0.f;  // LFO frequency in Hz (0 = LFO OFF)
  float morph_amt = 0.f; // LFO morph depth (0..1 LINEAR, 0 = off)
  uint8_t shape = kShapeSin; // LFO shape (0=SIN, 1=S, 2=WALK)

  // ---- WALK scene state (only used when shape == kShapeWalk) ----
  float gap_sec = 30.f; // mean seconds between points (LOG 1..300 s)
  uint8_t  scene_npts = 0;
  float    scene_t[kWalkMaxPoints];
  float    scene_p[kWalkMaxPoints];
  // Crossfade state: when a scene is regenerated (cycle boundary OR a live
  // GAP / RATE / SHAPE change), the output blends from `start_val_` (the
  // captured pre-regeneration output) to the NEW scene over kWalkXfadeSec.
  // xfade goes 0 -> 1 (old -> new); at 1 the blend is over (single lookup).
  float start_val_ = 0.f;
  float xfade = 1.f; // 1 = no fade in flight (steady state, zero overhead)
  bool  scene_dirty = true;
  uint32_t rng_state_ = 0x12345678u;

  // Crossfade time (seconds) for swapping scenes after a live GAP / RATE /
  // SHAPE change: long enough to be inaudible as a click, short enough that
  // a rapid knob drag keeps regenerating from the (already fading) current
  // output. 0.3 s.
  static constexpr float kWalkXfadeSec = 0.3f;

  // ---- Audio path ----
  float process()
  {
    if (freq_hz > 0.f)
    {
      phase += freq_hz * (1.f / kSampleRate);
      if (phase >= 1.f)
      {
        phase -= 1.f;
        // Cycle boundary (walk phase wraps): always regenerate, and capture
        // the current output as the new start value for a click-free swap.
        if (shape == kShapeWalk)
        {
          capture_start();
          regenerate_scene();
          scene_dirty = false;
          xfade = 0.f;
        }
      }
    }
    if (morph_amt <= 0.f)
      return 0.f;
    if (shape == kShapeSin)
      return osc_sinf(phase) * morph_amt;
    if (shape == kShapeS)
    {
      const float two_p = 2.f * phase;
      const float tri = (two_p < 1.f) ? two_p : (2.f - two_p);
      const float warped = tri + 0.3f * osc_sinf(tri);
      return (warped / 1.47f - 0.5f) * 2.f * morph_amt;
    }
    // WALK.
    if (scene_dirty)
    {
      // Live parameter change (GAP / RATE / SHAPE, via invalidate() from the
      // apply path): regenerate IMMEDIATELY so the new point density is
      // audible this instant, and crossfade from the captured current output
      // so the swap is click-free. The swap happens mid-cycle (NOT at the
      // boundary): the new scene is a full 0->1 story that we simply start
      // reading from the current phase, so it always ends at 0 at the cycle
      // boundary (regenerate_scene pins both endpoints at 0).
      capture_start();
      regenerate_scene();
      scene_dirty = false;
      xfade = 0.f;
    }
    // Advance the crossfade clock (once per sample; ~0.3 s to complete).
    if (xfade < 1.f)
    {
      xfade += (1.f / (kWalkXfadeSec * kSampleRate));
      if (xfade > 1.f)
        xfade = 1.f;
    }
    const float vnew = scene_lookup(phase);
    if (xfade >= 1.f)
      return vnew * morph_amt;
    return (vnew * xfade + start_val_ * (1.f - xfade)) * morph_amt;
  }

  // Capture the current (pre-regeneration) output value at the current phase
  // as the start of the crossfade. During an in-progress fade the output is
  // a blend of scene_ and start_val_, so capture that exact blend.
  void capture_start()
  {
    if (scene_npts > 1)
    {
      const float vnew = scene_lookup(phase);
      if (xfade < 1.f)
        start_val_ = vnew * xfade + start_val_ * (1.f - xfade);
      else
        start_val_ = vnew;
    }
    else
    {
      start_val_ = 0.f;
    }
  }

  void reset()
  {
    phase = 0.f;
    scene_dirty = true;
    rng_state_ = 0x12345678u;
  }

  // Force a scene regeneration on the NEXT process() call (used by the
  // apply_edit_param path when the LFO's freq / shape / gap change, so the
  // user hears the new point density IMMEDIATELY instead of waiting for the
  // next cycle boundary). At freq = 0.01 Hz a cycle is 100 s, so without
  // this a GAP change would take up to 100 s to become audible.
  void invalidate()
  {
    scene_dirty = true;
  }

  void regenerate_scene()
  {
    if (freq_hz <= 0.f)
    {
      scene_npts = 0;
      return;
    }
    const float period = 1.f / freq_hz;
    int n = static_cast<int>(period / gap_sec + 0.5f);
    if (n < 2) n = 2;
    if (n > kWalkMaxPoints) n = kWalkMaxPoints;
    // Reserve 4 slots for the guaranteed extremes (so n is the TOTAL count).
    // We need: [endpoint, +1.0, endpoint, -1.0, endpoint] + random points.
    // Minimum total = 5 (1 endpoint + 2 extremes + 1 endpoint + 1 random).
    if (n < 5) n = 5;
    if (n > kWalkMaxPoints) n = kWalkMaxPoints;
    scene_npts = static_cast<uint8_t>(n);
    // Seamless loop: first and last points must have the SAME value (so the
    // cycle wraps without a click). Draw that shared value randomly in
    // [-1, +1] so the walk can start/end at ANY level (not just 0).
    rng_state_ = rng_state_ * 1664525u + 1013904223u;
    const float endpoint = static_cast<float>(rng_state_ >> 8) / 2147483648.f * 2.f - 1.f;
    // Guaranteed extremes: the walk MUST reach +1.0 AND -1.0 every cycle
    // (like Sin/S, which hit their peaks/troughs every cycle). This ensures
    // the full bank is swept at Depth=100%, regardless of the random points.
    // Order: [endpoint, +1.0, endpoint, -1.0, endpoint] — the extremes are
    // placed at random times (exponential gaps), so the "visit" to each
    // extreme happens at a different moment every cycle.
    scene_t[0] = 0.f;
    scene_p[0] = endpoint;
    // Fill the remaining n-1 points: 2 are the extremes (+1.0, -1.0), the
    // rest are random in [-1, +1]. We draw n-1 random times (exponential
    // gaps), then assign: first two random times -> extremes (random order),
    // the rest -> random values.
    float exps[kWalkMaxPoints];
    float exp_sum = 0.f;
    for (int i = 0; i < n - 1; i++)
    {
      rng_state_ = rng_state_ * 1664525u + 1013904223u;
      float u = static_cast<float>(rng_state_ >> 8) / 2147483648.f;
      float uu = 0.5f * (u + 1.0f);
      if (uu < 1e-6f) uu = 1e-6f;
      float e = -fasterlogf(uu);
      exps[i] = e;
      exp_sum += e;
    }
    if (exp_sum < 1e-6f) exp_sum = 1e-6f;
    float t_cum = 0.f;
    // Assign times to points 1..n-1 (point n is the endpoint at t=1).
    for (int i = 0; i < n - 1; i++)
    {
      t_cum += exps[i] / exp_sum;
      scene_t[i + 1] = t_cum;
    }
    // Assign values: point n-1 (last before endpoint) is -1.0, point n-2 is
    // +1.0 (or swapped, randomly). The rest are random in [-1, +1].
    rng_state_ = rng_state_ * 1664525u + 1013904223u;
    const bool swap_extremes = (rng_state_ >> 16) & 1U;
    for (int i = 1; i < n; i++)
    {
      if (i == n - 1)
        scene_p[i] = swap_extremes ? 1.0f : -1.0f; // last interior: one extreme
      else if (i == n - 2)
        scene_p[i] = swap_extremes ? -1.0f : 1.0f;  // second-to-last: other extreme
      else
      {
        rng_state_ = rng_state_ * 1664525u + 1013904223u;
        scene_p[i] = static_cast<float>(rng_state_ >> 8) / 2147483648.f * 2.f - 1.f;
      }
    }
    // The endpoint (point n) is the same as point 0 (seamless loop).
    scene_t[n - 1] = 1.f;
    scene_p[n - 1] = endpoint;
  }

  float scene_lookup(float x) const
  {
    if (scene_npts < 2)
      return 0.f;
    const int n = scene_npts;
    int i = 0;
    while (i < n - 1 && scene_t[i + 1] <= x)
      i++;
    if (i >= n - 1)
      return scene_p[n - 1];
    const int i0 = (i > 0) ? i - 1 : 0;
    const int i1 = i;
    const int i2 = (i + 1 < n) ? i + 1 : n - 1;
    const int i3 = (i + 2 < n) ? i + 2 : n - 1;
    const float p0 = scene_p[i0], p1 = scene_p[i1];
    const float p2 = scene_p[i2], p3 = scene_p[i3];
    const float t1 = scene_t[i1], t2 = scene_t[i2];
    float u;
    if (t2 > t1)
      u = (x - t1) / (t2 - t1);
    else
      u = 0.f;
    if (u < 0.f) u = 0.f;
    if (u > 1.f) u = 1.f;
    const float u2 = u * u;
    const float u3 = u2 * u;
    return 0.5f * ((2.f * p1) +
                   (-p0 + p2) * u +
                   (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * u2 +
                   (-p0 + 3.f * p1 - 3.f * p2 + p3) * u3);
  }
};
