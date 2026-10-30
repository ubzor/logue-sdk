#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_envelope.h
 *
 *  Single-voice A-H-R (Attack-Hold-Release) envelope for the DRONE unit.
 *
 *  The NTS-1 mkII platform applies its own amplitude EG, but the device EG
 *  mode is set to oPEn (no platform shaping), so this unit runs its own
 *  envelope:
 *
 *    Attack   2 s, exponential fade 0 -> 1
 *    Hold     full level while the note is active
 *    Release  2 s, exponential fade 1 -> 0, then silent
 *
 *  The note "active" state is managed by the Osc (noteOn / noteOff /
 *  allNoteOff call noteOn() / noteOff() / allOff() here). The envelope is
 *  advanced one sample at a time from the Osc's audio path via advance().
 *
 */

struct AhEnvelope
{
  // A-H-R phase (single voice)
  enum Phase : uint8_t
  {
    kIdle = 0,     // silent
    kAttack,       // 0 -> 1 exponential
    kHold,         // full level while note active
    kRelease       // 1 -> 0 exponential, then idle
  };

  // Phase lengths in samples (2 s @ 48 kHz)
  static constexpr uint32_t kAttackSamples  = 2U * 48000U; // 2 s @48 kHz
  static constexpr uint32_t kReleaseSamples = 2U * 48000U; // 2 s @48 kHz

  Phase phase = kIdle;
  uint32_t elapsed = 0; // samples elapsed in the current phase
  float env = 0.f;      // current envelope level, 0..1

  // ---- Note control (called from Osc::noteOn / noteOff / allNoteOff) ----

  // Key press: note active. Start (or restart) the A-H-R sequence from the top
  // of the attack. A second key press while a sequence is still running
  // restarts it (re-trigger), which is the expected drone behavior.
  void noteOn()
  {
    phase = kAttack;
    elapsed = 0;
    env = 0.f;
  }

  // Key release: note inactive. If the envelope is in Attack or Hold, begin
  // the 2 s exponential release. If already in Release or Idle, nothing
  // happens (the release runs to completion on its own).
  void noteOff()
  {
    if (phase == kAttack || phase == kHold)
    {
      phase = kRelease;
      elapsed = 0;
    }
  }

  // All-notes-off: kill any in-flight envelope immediately (instant silence —
  // no release tail).
  void allOff()
  {
    phase = kIdle;
    elapsed = 0;
    env = 0.f;
  }

  // Stop the note and kill the envelope (reset / allNoteOff semantics).
  void reset() { allOff(); }

  // ---- Audio path (called once per sample from Osc::process) ----

  // Advance the envelope by one sample.
  //
  // alpha_a / alpha_r are per-sample first-order relaxation coefficients,
  // precomputed by the Osc in init() (never in the audio path):
  //   alpha = 1 - exp(-5 / N)  where N is the phase length in samples.
  void advance(float alpha_a, float alpha_r)
  {
    switch (phase)
    {
    case kAttack:
      // exponential fade 0 -> 1: env += (1 - env) * alpha_a
      env += (1.f - env) * alpha_a;
      if (++elapsed >= kAttackSamples)
      {
        elapsed = 0;
        phase = kHold;
        env = 1.f; // snap to full at the attack boundary
      }
      break;
    case kHold:
      // Full level while the note is active. If the note was released
      // (noteOn not re-firing), noteOff() will have switched us to Release;
      // if we are here, the note is still held.
      env = 1.f;
      break;
    case kRelease:
      // exponential fade 1 -> 0: env -= env * alpha_r
      env -= env * alpha_r;
      if (++elapsed >= kReleaseSamples)
      {
        phase = kIdle;
        env = 0.f;
      }
      break;
    default: // kIdle
      env = 0.f;
      break;
    }
  }
};
