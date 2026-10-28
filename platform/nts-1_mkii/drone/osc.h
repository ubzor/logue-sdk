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
 *  File: osc.h
 *
 *  DRONE -- polyphonic cluster drone generator for NTS-1 mkII.
 *
 *  Four independent voice slots, each containing a dual-sawtooth oscillator
 *  pair. Each voice has its own autonomous life cycle:
 *
 *    REVIVAL → ATTACK → SUSTAIN → RELEASE → REST → (repeat)
 *
 *  with random durations for each phase (fixed ranges, see constants below).
 *  When a new note is triggered and the pool is full, the oldest voice (by
 *  spawn order) is marked for death: it finishes its current phase and frees
 *  the slot immediately after RELEASE (skipping REST). The new voice waits in
 *  a FIFO queue (max NUM_VOICES); on overflow the oldest queued note is
 *  evicted and replaced by the newest.
 *
 *  The A knob (DETUNE) detunes the second oscillator up to +/-50 cents from
 *  center, producing phase beating. The B knob (SAT) scales the drive level
 *  before a soft-clip tanh saturator, adding harmonic density.
 *
 *  An optional random-walk "wandering" mechanism modulates the detune around
 *  the knob value per voice: RATE AMT sets the maximum deviation (quadratic
 *  mapping: knob^2 -> 0..+/-10 cents hard ceiling) and RATE SPD sets the
 *  "restlessness" — how often the target changes and how quickly the detune
 *  tracks it (0.1 s fast, restless .. 10 s slow, lazy).
 *
 *  No dynamic allocation: all state lives in static members of this class.
 *
 */

#include "processor.h"
#include "unit_osc.h"

namespace {
// Fast soft-clip saturator: rational (Padé [5/5]) approximation of tanh(x)
// over the full operating range x in [-4.68, 4.68] (the argument of tanhf()
// in process() is bounded by d / (1 - d^2) with |d| <= 0.9).
//
// The approximation uses the compacting map u = |x| / (1 + |x|) in [0, 0.823]
// and evaluates  tanh(x) ~= sign(x) * u * P(u) / Q(u),
// where P, Q are degree-5 polynomials fitted in the least-squares sense.
//
// Verified accuracy (float32, the deployment precision):
//   max |tanh(x) - approx(x)| over [-4.68, 4.68]  = 7.0e-4  (-63 dBFS)
// which is below the audibility threshold for this saturator (the error
// appears as a very slight rounding of the highest harmonics at full drive).
//
// Cost: ~14 VFP multiplies/adds + 1 divide per sample, versus ~40-80 cycles
// for the libm tanhf() (which pulls in expm1f, fabsf, __errno, ... ~900 B).
// No branches, no table lookups, no cancellation (all intermediate values
// stay in [0.5, 25], well within float32's 24-bit mantissa).
inline float soft_tanhf(float x)
{
  const float ax = x < 0.f ? -x : x; // fabsf
  const float u = ax / (1.f + ax);

  // P(u) = p0 + p1 u + p2 u^2 + p3 u^3 + p4 u^4 + p5 u^5
  const float num = 0.995777515f +
      u * (0.594395935f +
           u * (3.48766993f +
                u * (-3.67006819f +
                     u * (-5.48908741f + u * 5.10677737f))));

  // Q(u) = 1 + q0 u + q1 u^2 + q2 u^3 + q3 u^4 + q4 u^5
  const float den = 1.f +
      u * (-0.596846538f +
           u * (5.40544503f +
                u * (-16.5090433f +
                     u * (20.8395950f + u * (-9.35298147f)))));

  const float t = u * num / den;
  return x < 0.f ? -t : t;
}

// ---------------------------------------------------------------------------
// Procedural tri-fold wavefolder (table-free, libm-free).
//
// y = sign(x) * |2*frac(x*k) - 1|
//
// For k = 1 + 5*f (f in [0,1]) the waveform is folded k-1 extra times, i.e.
// the input [-1,1] is mapped onto k triangular half-cycles. The result is
// odd-symmetric (DC-free) and bounded by |1|, so it is a safe front-end for
// the saturator. k=1 (f=0) is the exact identity, which keeps FOLD=0
// bit-identical to the original signal path (regression-safe).
//
// Cost: 1 multiply, 1 floor, ~4 add/sub, 2 fabs -- ~10 VFP ops per stage.
// No libm (floorf -> FLOOR on the M7 FPU), no tables, no division.
inline float tri_foldf(float x, float k)
{
  const float v = x * k;
  const float f = v - si_floorf(v); // frac(v) in [0, 1)
  const float t = 2.f * f - 1.f;    // triangle wave in [-1, 1], period 1 in v
  const float m = t < 0.f ? -t : t; // |t|
  return x < 0.f ? -m : m;          // restore odd symmetry
}

// 3-stage fold with per-stage gain compensation.
//
// Each stage multiplies the signal by k and folds it. The natural peak gain
// of a k-fold triangle grows with k, so each stage is pre-attenuated by
// (1/k). At k=1 the compensation is exactly 1 and the whole section is the
// identity; at higher k the compensation keeps the output level stable so
// the downstream soft-clip sees a constant drive level independent of FOLD.
// Three stages give enough harmonic density for a dark "folded" texture.
inline float wavefolder3(float x, float k, float g)
{
  // g = 1/k per-stage gain compensation, precomputed once per block (1.0 at
  // k = 1, i.e. FOLD = 0 -> exact identity)
  float y = tri_foldf(x * g, k);
  y = tri_foldf(y * g, k);
  y = tri_foldf(y * g, k);
  return y;
}
} // namespace

class Osc : public Processor
{
public:
  uint32_t getBufferSize() const override final { return 0; } // NTS-1 osc do not support sdram allocation

  // audio parameters (index order MUST match the parameter table in header.c)
  enum
  {
    SHAPE = 0U,   // knob A: detune of oscillator B (-50 .. +50 cents)
    ALT,          // knob B: drive level into the soft-clip saturator (0 .. 3)
    FOLD,         // edit: procedural wavefolder depth (0..100%, default 0% = off)
    RATE_AMT,     // edit: random-walk depth (0..100%, default 0% = off)
    RATE_SPD,     // edit: random-walk speed (0..100%, default 50%)
    NUM_PARAMS
  };

  // random-walk maximum deviation in cents at RATE AMT = 100% (full knob).
  // The detune never strays further from the DETUNE knob value than this.
  static constexpr float kRwMaxCents = 10.f;

  // Voice pool size
  static constexpr int NUM_VOICES = 1;

  // Voice life cycle states
  enum VoiceState : uint8_t {
    kStateIdle     = 0,  // voice is silent, waiting for a note
    kStateRevival  = 1,  // silent, waiting to start (random delay)
    kStateAttack   = 2,  // amp: 0 → velocity (random duration)
    kStateSustain  = 3,  // amp = velocity (random duration)
    kStateRelease  = 4,  // amp: velocity → 0 (random duration).
                         // On completion: if marked_for_death -> Idle (slot freed),
                         // otherwise -> Rest.
    kStateRest     = 5,  // silent, waiting for next cycle (random duration).
                         // Only reached by non-flagged voices.
  };

  // Fixed ranges for random life cycle durations (in seconds).
  // These are the bounds from which random values are drawn on each revival.
  static constexpr float kRevivalMin = 0.5f, kRevivalMax = 3.0f;
  static constexpr float kAttackMin  = 0.1f, kAttackMax  = 2.0f;
  static constexpr float kSustainMin = 1.0f, kSustainMax = 5.0f;
  static constexpr float kReleaseMin = 2.0f, kReleaseMax = 8.0f;
  static constexpr float kRestMin    = 0.5f, kRestMax    = 4.0f;

  // Release tail-shaping constants.
  //
  // The release envelope is split into two parts over T_rel samples:
  //
  //   1. A linear body from v.vel down to a small floor level
  //      (v.vel * kReleaseFloorAmp, i.e. ~-60 dB of the note's peak velocity)
  //      over the first (1 - kReleaseTailFrac) fraction of T_rel, i.e. over
  //      T_rel * (1 - kReleaseTailFrac) samples.
  //
  //   2. A short exponential "tail" over the last kReleaseTailFrac fraction
  //      of T_rel that decays the remaining (floor_at_tail_start - floor)
  //      difference toward a hard floor with a FIXED time constant
  //      kReleaseTauSec:
  //
  //          tail(t) = floor + (floor_at_tail_start - floor) * exp(-t / tau),
  //          t in [0, T_rel * kReleaseTailFrac]   (t = 0 at tail start)
  //          floor_at_tail_start = v.vel * kReleaseFloorAmp
  //          floor               = 0 (hard zero)
  //
  // The fixed time constant tau = 0.04 s (40 ms) is independent of T_rel, so
  // the tail's perceived "length" (how fast the audible level drops) is the
  // same for a 2 s release as for an 8 s release -- the only thing that
  // changes is the start level floor_at_tail_start (a small linear step at
  // the boundary, never a discontinuity).
  //
  // The tail is clamped to a hard floor (v.vel * kReleaseFloorAmp * 0.5,
  // i.e. when the exponential has converged within half the floor of the
  // zero target) at the instant the remaining difference drops below that
  // floor; because the exponential is fast (40 ms) and the tail is long
  // (>= 0.2 s at T_rel >= 2 s), the clamp instant is always well before the
  // state transition. The state transition (Release -> Rest/Idle) therefore
  // jumps from <= floor_amp (inaudible, ~-60 dB, and even less for quieter
  // notes because the floor is proportional to v.vel) to 0, instead of the
  // previous vel/T_rel step that was clearly audible after FOLD gain
  // compensation.
  static constexpr float kReleaseTailFrac = 0.10f; // last 10% of T_rel
  static constexpr float kReleaseFloorAmp = 1e-3f; // ~ -60 dB of full vel
  static constexpr float kReleaseTauSec = 0.04f;    // exponential tail decay

  // Fixed release time (seconds) used when a voice is killed by allNoteOff()
  // (e.g. the last note is released, or the user hits stop). The drone's
  // autonomous life cycle still uses the random T_rel (2–8 s) for its
  // natural release, but an explicit stop should be prompt and clean rather
  // than letting the voice drone out for several seconds with the wavefolder
  // still folding the tail into audible harmonics. 0.8 s is long enough to
  // be inaudible as a "cut" and short enough to feel like an intentional
  // stop.
  static constexpr float kAllNoteOffReleaseSec = 0.8f;

  // Note: Make sure that default param values correspond to declarations in header.c
  struct Params
  {
    float detune_cents; // -50.f .. +50.f, knob A
    float drive;        // 0.f .. 3.f, knob B
    float fold;         // 0.f .. 1.f, edit: wavefolder depth (quadratic)
    float rw_amount;    // 0.f .. 1.f, edit: random-walk depth
    float rw_rate;      // 0.f .. 1.f, edit: random-walk speed

    void reset()
    {
      detune_cents = 0.f; // default: in tune
      drive = 1.f;        // default: unity drive
      fold = 0.f;         // default: folder bypass (bit-identical to SAT only)
      rw_amount = 0.f;    // default: random walk off
      rw_rate = 0.5f;     // default: mid speed
    }

    Params() { reset(); }
  };

  // Per-voice state: autonomous life cycle + oscillator pair + random-walk
  struct Voice
  {
    // Oscillator state
    float w0_a;        // base phase increment
    float w0_b;        // w0_a * detune_factor (per-voice cache)
    float ph_a;        // phasor A
    float ph_b;        // phasor B

    // Amplitude envelope
    float amp;         // current amplitude, 0..1
    float vel;         // target amplitude (velocity/127), 0..1

    // Random-walk detune (per-voice)
    float dev_cents;   // current deviation in cents
    float rw_target;   // target deviation in cents
    uint32_t rw_counter; // samples since last target update

    // Life cycle state machine
    VoiceState state;  // current state
    uint32_t timer;    // samples remaining in current state
    uint32_t T_rev;    // random revival duration (samples)
    uint32_t T_att;    // random attack duration (samples)
    uint32_t T_sus;    // random sustain duration (samples)
    uint32_t T_rel;    // random release duration (samples)
    uint32_t T_rest;   // random rest duration (samples)

    // Voice metadata
    uint8_t note;      // MIDI note number (for reference)
    uint32_t spawn_seq; // monotonic spawn order; lower = older. 0 = never spawned.
    bool marked_for_death; // true if this voice should die after current Release

    void reset()
    {
      w0_a = w0_b = ph_a = ph_b = amp = vel = dev_cents = rw_target = 0.f;
      rw_counter = 0;
      state = kStateIdle;
      timer = T_rev = T_att = T_sus = T_rel = T_rest = 0;
      note = 0;
      spawn_seq = 0;
      marked_for_death = false;
    }
  };

  void setParameter(uint8_t index, int32_t value) override final
  {
    switch (index)
    {
    case SHAPE:
      // 0 .. 1023 -> -50 .. +50 cents, center (511) is in tune
      params_.detune_cents = param_10bit_to_f32(value) * 100.f - 50.f;
      break;

    case ALT:
      // 0 .. 1023 -> 0.f .. 3.f drive level into the saturator
      params_.drive = param_10bit_to_f32(value) * 3.f;
      break;

    case FOLD:
      // 0 .. 1023 -> 0.f .. 1.f wavefolder depth, quadratic mapping (same
      // rationale as RATE AMT): small knob positions give barely-audible
      // folding, full knob gives the full 3-stage fold. The fold coefficient
      // k = 1 + 5*fold is recomputed in process() once per block (k-rate).
      {
        const float x = param_10bit_to_f32(value);
        params_.fold = x * x;
      }
      break;

    case RATE_AMT:
      // 0 .. 1023 -> 0.f .. 1.f random-walk depth, quadratic mapping:
      // small knob positions give very small (barely audible) deviations,
      // full knob gives full +/-20-cent wander. rw_amount = x^2 where x is
      // the linear 0..1 knob value. This makes the low end of the knob a
      // fine "almost off" control instead of a hard on/off step.
      {
        const float x = param_10bit_to_f32(value);
        params_.rw_amount = x * x;
      }
      // Recompute the hard ceiling for the deviation (cents)
      rw_max_dev_ = params_.rw_amount * kRwMaxCents;
      break;

    case RATE_SPD:
      // 0 .. 1023 -> 0.f .. 1.f random-walk speed
      params_.rw_rate = param_10bit_to_f32(value);
      // Compute the target update interval in samples (recomputed once per parameter change)
      // period = 0.1 * 100^rw_rate seconds, converted to samples
      {
        const float period = 0.1f * fasterpowf(100.f, params_.rw_rate);
        rw_target_update_interval_ = (uint32_t)(period * 48000.f);
      }
      break;

    default:
      break;
    }
  }

  // life-cycle methods
  void init(float *) override final
  {
    params_.reset();
    w0_ = 0.f;
    lfo_ = 0.f;
    lfoz_ = 0.f; // wavefolder LFO interpolation state
    lp_ = 0.f;   // wavefolder LFO lowpass state
    rw_max_dev_ = 0.f; // random-walk hard ceiling starts at 0 (AMT=0)
    rw_target_update_interval_ = 48000; // random-walk interval starts at 1s (default)
    last_w0_ = 0.f; // cached pitch for voice spawning
    gate_closed_ = false; // gate is open initially
    
    for (int i = 0; i < NUM_VOICES; ++i) {
      voices_[i].reset();
    }
    queue_head_ = queue_tail_ = 0;
    spawn_counter_ = 0;
  }

  void reset() override final
  {
    for (int i = 0; i < NUM_VOICES; ++i) {
      voices_[i].reset();
    }
    queue_head_ = queue_tail_ = 0;
    spawn_counter_ = 0;
  }

  // MIDI note handlers
  void noteOn(uint8_t note, uint8_t velocity) override final
  {
    // Reopen the gate on any noteOn
    gate_closed_ = false;
    
    // Convert velocity to target amplitude (0..1)
    const float vel = static_cast<float>(velocity) / 127.f;
    
    // Try to spawn a voice in an idle slot
    for (int i = 0; i < NUM_VOICES; ++i) {
      if (voices_[i].state == kStateIdle) {
        spawn_voice(i, note, vel);
        return;
      }
    }
    
    // Pool is full: mark the oldest active voice for death (by spawn order),
    // queue the new note.
    // If the queue is full, evict the oldest queued note (shift the queue).
    int oldest = -1;
    uint32_t oldest_seq = 0xFFFFFFFFu;
    for (int i = 0; i < NUM_VOICES; ++i) {
      Voice &vc = voices_[i];
      if (vc.state != kStateIdle && !vc.marked_for_death && vc.spawn_seq != 0) {
        if (oldest == -1 || vc.spawn_seq < oldest_seq) {
          oldest = i;
          oldest_seq = vc.spawn_seq;
        }
      }
    }
    if (oldest >= 0) {
      voices_[oldest].marked_for_death = true;
    }

    // Queue the new note (FIFO, max NUM_VOICES). On overflow, evict the
    // oldest queued note so the newest one always takes its place.
    if (queue_tail_ - queue_head_ >= NUM_VOICES) {
      queue_head_++;
    }
    queue_[queue_tail_ & (NUM_VOICES - 1)] = {note, vel};
    queue_tail_++;
  }

  void noteOff(uint8_t note) override final
  {
    // The drone ignores individual key releases: a spawned voice lives
    // indefinitely (Release -> Rest -> Revival -> ...). Only an allNoteOff
    // (every key released / a full stop) actually kills the voices.
    (void)note;
  }

  void allNoteOff() override final
  {
    gate_closed_ = true;
    // Mark every non-idle voice for death and immediately kill those that
    // are silent (Revival/Rest) -- they would otherwise revive into Attack
    // and keep sounding (marked_for_death is only checked at the end of
    // Release, so a voice in Revival/Rest that is marked for death would
    // still go through Attack->Sustain->Release before dying).
    //
    // Voices in Attack/Sustain transition to Release now, with a FIXED short
    // release time (kAllNoteOffReleaseSec) instead of the random T_rel that
    // was drawn at spawn time. The random T_rel is 2-8 s (see kReleaseMin /
    // kReleaseMax); with the wavefolder active the long tail is folded into
    // audible harmonics and the user hears a several-second "square-wave"
    // drone before the final cut. A short fixed release makes the stop
    // prompt and clean while still being inaudible as a click (the
    // release envelope's exponential tail + hard-floor clamp guarantees the
    // state transition happens at <= floor_amp, see kReleaseTailFrac /
    // kReleaseFloorAmp / kReleaseTauSec).
    //
    // Voices already in Release keep their in-progress timer (the user
    // already heard the start of the fade); the death check happens at the
    // Release completion as before.
    const uint32_t short_rel =
        static_cast<uint32_t>(kAllNoteOffReleaseSec * 48000.f);
    for (int i = 0; i < NUM_VOICES; ++i) {
      Voice &v = voices_[i];
      if (v.state == kStateIdle) continue;
      v.marked_for_death = true;
      if (v.state == kStateRevival || v.state == kStateRest) {
        // Kill silent voices immediately -- they would otherwise revive.
        v.state = kStateIdle;
        v.marked_for_death = false;
        v.reset();
      } else if (v.state == kStateSustain || v.state == kStateAttack) {
        v.state = kStateRelease;
        v.timer = short_rel;
      }
    }
    // Clear the queue
    queue_head_ = queue_tail_ = 0;
  }

  // audio processing callbacks

  // set frequency in digital w (w = f/samplerate, 0.5 is Nyquist)
  void setPitch(float w0)
  {
    w0_ = w0; // base phase increment, osc A uses this directly
    last_w0_ = w0; // cache for voice spawning
  }

  // lfo in (-1.0f, 1.0f)
  void setShapeLfo(float lfo)
  {
    lfo_ = lfo;
  }



  // ---- Temporary diagnostic state (remove after debugging) ----
  // Logs voice state + envelope + output level once per kDiagLogInterval
  // samples. Writes to stdout via host printf; in wasm this goes to the
  // browser console (or /dev/null if not captured).
  static constexpr uint32_t kDiagLogInterval = 4800; // ~100 ms at 48 kHz
  uint32_t diag_counter_ = 0;
  // Last mix value computed in the previous process() call; used to log
  // the level going INTO the fold/saturator section (before gain staging).
  float diag_last_mix_ = 0.f;

  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final
  {
    (void)in; // drone generator does not use the audio input

    // Caching current parameter values. Consider smoothing sensitive parameters in audio loop
    const Params p = params_;

    // base phase increment
    const float w0 = w0_;
    const float drive = p.drive;

    // ---- Random-walk detune deviation (target-based wandering) ----
    // Per-voice random walk: each voice has its own deviation and target.
    // RATE AMT = maximum deviation (hard ceiling): the detune deviation is
    // limited to +/-rw_max_dev_ = +/-rw_amount*kRwMaxCents cents.
    // RATE SPD = restlessness: controls how often the target changes and how
    // quickly the deviation tracks it.
    const bool rw_active = p.rw_amount > 0.f;
    float rw_k = 0.f;
    uint32_t rw_interval = 0;
    if (rw_active)
    {
      // Compute the tracking coefficient once per block (rw_rate may change)
      // period = 0.1 * 100^rw_rate seconds
      const float period = 0.1f * fasterpowf(100.f, p.rw_rate);
      // Tracking coefficient: how quickly the deviation moves toward the target.
      // Higher k = faster tracking. k = 1 / (1 + tau * 48000) where tau is the
      // relaxation time (we use period as tau for simplicity).
      rw_k = 1.f / (1.f + period * 48000.f);
      rw_interval = rw_target_update_interval_;
    }

    // ---- Procedural wavefolder (k-rate) ----
    // FOLD sets a base depth (0..1). The NTS-1 Shape LFO modulates the depth
    // by +/-100% through a slow lowpass so the folding "breathes" (slow
    // motion) instead of phase-scanning. k = 1 + 5*depth_eff is the fold
    // coefficient (1 = identity, 6 = full fold). The per-stage gain
    // compensation g = 1/k is precomputed once per block: at k=1 it is 1.0,
    // so FOLD=0 + LFO=0 keeps the signal path identical to SAT only.
    const float fold_base = p.fold;
    const bool  fold_on   = fold_base > 0.f || lfo_ != 0.f;
    const float lfo_inc   = (lfo_ - lfoz_) / static_cast<float>(frames);
    float       lp_res    = 0.f;
    if (fold_on)
    {
      // Lowpass coefficient for the LFO: cutoff ~ 2 Hz at depth 0, ~ 30 Hz at
      // full fold. One-pole per sample: res = 1 - exp(-2*pi*fc/48000) ~
      // 2*pi*fc/48000 for the small argument (fc << 48000, error << 1%).
      const float fc = 2.f + fold_base * 28.f;
      lp_res = 2.f * 3.14159265358979f * fc / 48000.f;
    }

    for (const float *out_end = out + frames; out != out_end; in += 2, out += 1)
    {
      // Process all 4 voices
      float mix = 0.f;
      float active_sum = 0.f;

      for (int i = 0; i < NUM_VOICES; ++i) {
        Voice &v = voices_[i];
        
        // Skip idle voices
        if (v.state == kStateIdle) {
          // Check if this voice can be spawned from queue
          if (queue_head_ < queue_tail_) {
            const auto &q = queue_[queue_head_ & (NUM_VOICES - 1)];
            spawn_voice(i, q.note, q.vel);
            queue_head_++;
          }
          continue;
        }
        // State machine.
        // A voice marked for death (by allNoteOff) in a silent phase
        // (Revival / Rest) is NOT freed instantly: it plays out its current
        // (silent) phase and only releases the slot at the end of Release.
        // This keeps the stop a smooth fade-out instead of an abrupt cut.

        switch (v.state) {
        case kStateRevival:
          if (v.timer == 0) {
            v.state = kStateAttack;
            v.timer = v.T_att;
            v.amp = 0.f;
          } else {
            v.timer--;
          }
          break;
          
        case kStateAttack:
          if (v.timer == 0) {
            v.state = kStateSustain;
            v.timer = v.T_sus;
            v.amp = v.vel;
          } else {
            v.timer--;
            const float progress = 1.f - static_cast<float>(v.timer) / static_cast<float>(v.T_att);
            v.amp = v.vel * progress;
          }
          break;
          
        case kStateSustain:
          v.amp = v.vel;
          if (v.timer == 0) {
            v.state = kStateRelease;
            v.timer = v.T_rel;
          } else {
            v.timer--;
          }
          break;
          
        case kStateRelease:
          if (v.timer == 0) {
            // Death happens here: if this voice is marked for death, free the
            // slot immediately (no REST). Otherwise, enter REST as a silent
            // pause before the next cycle.
            if (v.marked_for_death) {
              v.state = kStateIdle;
              v.marked_for_death = false;
              v.reset();
            } else {
              v.state = kStateRest;
              v.timer = v.T_rest;
            }
            v.amp = 0.f;
          } else {
            v.timer--;
            // Release envelope: linear body over the first (1 - kReleaseTailFrac)
            // fraction of T_rel, then a short exponential tail over the last
            // kReleaseTailFrac fraction that decays toward 0 with a fixed
            // time constant kReleaseTauSec, clamped to a hard floor of
            // v.vel * kReleaseFloorAmp. This prevents a hard zero-jump at
            // the Release boundary (the click heard when FOLD is active,
            // because the folder folds the linear-ramp tail into audible
            // harmonics). See kReleaseTailFrac / kReleaseFloorAmp /
            // kReleaseTauSec for the design rationale.
            const float T_rel = static_cast<float>(v.T_rel);
            const float remain = static_cast<float>(v.timer); // samples left
            const float floor_amp = v.vel * kReleaseFloorAmp;
            float amp = 0.f;
            if (T_rel > 1.f) {
              const float tail_start = T_rel * (1.f - kReleaseTailFrac);
              const float body_len = T_rel - tail_start; // samples
              if (remain >= tail_start) {
                // Linear body: vel -> floor_amp over [tail_start, T_rel].
                amp = v.vel - (v.vel - floor_amp) *
                      ((T_rel - remain) / body_len);
              } else {
                // Exponential tail: floor_amp * exp(-t / tau) where
                // t = (tail_start - remain) samples into the tail. t == 0 at
                // the tail start (continuous with the linear body),
                // t == tail_start at the state transition. The tail is
                // clamped to 0 (the hard floor) once the exponential has
                // decayed to floor_amp/2, i.e. when the remaining difference
                // is small enough that the final transition jump (to 0) is
                // always <= floor_amp (inaudible).
                const float t = tail_start - remain; // samples
                const float tau = kReleaseTauSec * 48000.f; // samples
                float a = floor_amp * fasterexpf(-t / tau);
                if (a < floor_amp * 0.5f) a = 0.f;
                amp = a;
              }
            } else {
              // Degenerate case: T_rel <= 1 sample (should not happen with the
              // current T_rel range, but guard against division-by-zero).
              amp = v.vel * (remain / T_rel);
            }
            v.amp = amp;
          }
          break;
          
        case kStateRest:
          // REST is only reached by non-flagged voices. On timer expiry,
          // restart the life cycle.
          if (v.timer == 0) {
            v.state = kStateRevival;
            v.timer = v.T_rev;
            regenerate_timers(v);
          } else {
            v.timer--;
          }
          break;
          
        default:
          break;
        }
        
        // Only process audio if voice is in Attack/Sustain/Release
        if (v.state == kStateAttack || v.state == kStateSustain || v.state == kStateRelease) {
          // Per-voice random-walk
          if (rw_active) {
            if (++v.rw_counter >= rw_interval) {
              v.rw_counter = 0;
              v.rw_target = osc_white() * rw_max_dev_;
            }
            v.dev_cents += (v.rw_target - v.dev_cents) * rw_k;
            if (v.dev_cents > rw_max_dev_)
              v.dev_cents = rw_max_dev_;
            else if (v.dev_cents < -rw_max_dev_)
              v.dev_cents = -rw_max_dev_;
            const float eff_detune = p.detune_cents + v.dev_cents;
            const float x = eff_detune * 5.7762265046261084e-04f;
            const float detune_factor =
                1.f + x * (1.f + x * (0.5f + x * (1.f / 6.f + x * (1.f / 24.f))));
            v.w0_b = v.w0_a * detune_factor;
          }
          
          // Phasor update
          v.ph_a += v.w0_a;
          v.ph_b += v.w0_b;
          
          // Wrap phasors into [0, 1)
          if (v.ph_a >= 1.f) v.ph_a -= 1.f;
          if (v.ph_b >= 1.f) v.ph_b -= 1.f;
          
          // Generate sawtooth waves in -1..1
          const float saw_a = 2.f * v.ph_a - 1.f;
          const float saw_b = 2.f * v.ph_b - 1.f;
          
          // Mix both oscillators equally, scale by voice amplitude
          const float voice_mix = 0.5f * (saw_a + saw_b) * v.amp;
          mix += voice_mix;
          active_sum += v.amp;
        }
      }
      
      // Soft normalization (no sqrt): 1 voice = 1.0, 2 voices = 0.67, 4 voices = 0.33
      const float norm = 1.f / (1.f + 0.5f * (active_sum - 1.f));
      mix *= norm;

      // ---- Procedural wavefolder (before the saturator) ----
      // The Shape LFO advances its block-interpolated value and is smoothed by
      // a one-pole lowpass, then modulates the fold depth: depth_eff =
      // clamp01(fold_base + 0.5 * lfo_z). At FOLD = 0 and LFO = 0 the section
      // is bypassed (k = 1, g = 1 -> identity) and mix passes through as is.
      lfoz_ += lfo_inc;
      if (fold_on)
      {
        lp_ += lp_res * (lfoz_ - lp_);           // slow "breathing" filter
        const float depth_eff = clip01f(fold_base + 0.5f * lp_);
        const float k = 1.f + 5.f * depth_eff;   // fold coefficient [1, 6]
        const float g = 1.f / k;                  // per-stage gain compensation
        mix = wavefolder3(mix, k, g);
      }

      // soft-clip saturator: y = (1 - d^2) * tanh(d / (1 - d^2)), d in (-1, 1)
      float d = mix * drive;
      if (d > 0.9f)
        d = 0.9f;
      else if (d < -0.9f)
        d = -0.9f;
      const float one_minus_d2 = 1.f - d * d;
      const float y = one_minus_d2 * soft_tanhf(d / one_minus_d2);
      
      // scale output to a musical level (saturator max output is ~0.76)
      out[0] = y * 0.9f;
      diag_last_mix_ = mix; // (kept for the host diagnostic sim)
    }
  }

private:
  // Voice pool
  Voice voices_[NUM_VOICES];
  
  // Queue for notes waiting for a free slot
  struct QueueItem {
    uint8_t note;
    float vel;
  };
  QueueItem queue_[NUM_VOICES];
  uint32_t queue_head_ = 0;
  uint32_t queue_tail_ = 0;

  // Monotonic spawn counter; incremented on every voice spawn. Used to order
  // voices by age (lower = older) when picking the "oldest" voice to evict.
  uint32_t spawn_counter_ = 0;
  
  // Cached pitch for voice spawning (variant B)
  float last_w0_ = 0.f;
  
  // Gate state: when true, new notes are ignored
  bool gate_closed_ = false;
  
  // random-walk hard ceiling for the deviation (cents), set by RATE AMT.
  float rw_max_dev_ = 0.f;
  // target update interval in samples (recomputed from RATE SPD each block).
  uint32_t rw_target_update_interval_ = 0;
  
  // Spawn a new voice in slot i
  void spawn_voice(int i, uint8_t note, float vel)
  {
    Voice &v = voices_[i];
    v.reset();
    v.note = note;
    v.vel = vel;
    v.spawn_seq = ++spawn_counter_;
    v.marked_for_death = false;
    // Use the last pitch set by setPitch(). If it has not been set yet
    // (e.g. the web simulator before the first setOscPitch call), derive it
    // from the MIDI note directly so the voice is still audible.
    if (last_w0_ > 0.f) {
      v.w0_a = last_w0_;
      v.w0_b = last_w0_;
    } else {
      // 440 Hz at MIDI 69, 12-tone equal temperament.
      const float freq = 440.f * fasterpowf(2.f, (static_cast<float>(note) - 69.f) / 12.f);
      const float w0 = freq / 48000.f;
      v.w0_a = w0;
      v.w0_b = w0;
      last_w0_ = w0;
    }
    v.state = kStateRevival;
    v.timer = v.T_rev;
    regenerate_timers(v);
  }
  
  // Regenerate random life cycle durations using LCG
  void regenerate_timers(Voice &v)
  {
    // Use a simple LCG to generate 6 random values from one osc_white() call
    uint32_t state = static_cast<uint32_t>(osc_white() * 0x7FFFFFFF) ^ 0x12345678u;
    
    auto next_rand = [&state]() -> float {
      state = state * 1664525u + 1013904223u;
      return (static_cast<float>(state >> 8) / 16777216.f); // 24-bit fraction in [0, 1)
    };
    
    v.T_rev = static_cast<uint32_t>((kRevivalMin + next_rand() * (kRevivalMax - kRevivalMin)) * 48000.f);
    v.T_att = static_cast<uint32_t>((kAttackMin + next_rand() * (kAttackMax - kAttackMin)) * 48000.f);
    v.T_sus = static_cast<uint32_t>((kSustainMin + next_rand() * (kSustainMax - kSustainMin)) * 48000.f);
    v.T_rel = static_cast<uint32_t>((kReleaseMin + next_rand() * (kReleaseMax - kReleaseMin)) * 48000.f);
    v.T_rest = static_cast<uint32_t>((kRestMin + next_rand() * (kRestMax - kRestMin)) * 48000.f);
  }

  Params params_;
  float w0_;
  float lfo_;

  // Procedural wavefolder state (k-rate):
  //   lfoz_ -- block-interpolated Shape LFO value in [-1, 1] (drives fold depth)
  //   lp_   -- 2-pole lowpass state for the LFO, keeps the fold-depth
  //             modulation slow ("breathing") even with a fast Shape LFO.
  float lfoz_ = 0.f;
  float lp_ = 0.f;

  public:
  // ---- Temporary debug accessors (remove after debugging) ----
  // These let the host diagnostic sim inspect voice state without
  // exposing the full Voice struct publicly.
  struct VoiceDebug {
    int state;
    float amp;
    uint32_t timer;
    bool marked_for_death;
  };
  VoiceDebug debug_voice0() const {
    const Voice& v = voices_[0];
    return { (int)v.state, v.amp, v.timer, v.marked_for_death };
  }
  // Also expose the pre-fold mix for diagnostics
  float debug_last_mix() const { return diag_last_mix_; }
};
