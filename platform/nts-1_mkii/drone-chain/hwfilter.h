#pragma once
/*
 *  File: hwfilter.h
 *
 *  Web-simulator-only "hardware filter" block: emulates the physical filter
 *  that sits on the NTS-1 between the OSC module and the MOD module.
 *
 *  This class is NOT part of any ARM unit build. On the real device the
 *  platform routes OSC -> (filter) -> MOD itself and only exposes two
 *  placeholder fields (cutoff / resonance, both "Unused. Future", see
 *  common/unit_osc.h and the NTS-1 mkII README), so the SDK gives no
 *  reference implementation. The simulator therefore builds its own block
 *  here, wired into the Chain between the OSC upmix and the MOD stage
 *  (see chain.h, Chain::process).
 *
 *  Controls (all UI-driven, raw 10-bit unless noted):
 *    - TYPE : thru / high4 / high2 / band4 / band2 / low4 / low2 (default THRU).
 *             THRU is a dry wire: the block is completely out of the path.
 *             2-pole types are one ladder SVF (12 dB/oct); 4-pole types are
 *             two cascaded ladders (24 dB/oct). Tap: LOW = position (LP),
 *             BAND = velocity (BP), HIGH = in - position (HP).
 *    - CUTOFF : raw 0..1023 (default 0 = min = 1 Hz), log-mapped
 *             kCutoffMinHz .. kCutoffMaxHz (1 Hz .. 20 kHz).
 *    - RES    : raw 0..1023 (default 0 = min Q 0.5), linearly mapped
 *             [kQMin, kQMax].
 *    - SWEEP F    : 0..30 Hz (default 0 = off), a sine oscillator that
 *             modulates the cutoff.
 *    - SWEEP AMT  : UI -100..+100 (raw 0..1023, default 511 ~ 0), a +/-1
 *             octave (symmetric in dB) multiplicative wander around the
 *             cutoff knob value: fc_eff = fc_knob * 2^(dev * sin(phase)).
 *
 *  The block is STEREO (the chain upmixes the mono OSC to L/R before this
 *  point). Each channel has its own ladder states; the K-rate coefficients
 *  (cc / ae) are shared and recomputed only when a parameter changes.
 *  The sweep phase is single (a global LFO) and drives both channels.
 *
 *  Stability: cc is clamped to (0, 0.92] and ae to [0.18, 1] per ladder,
 *  exactly as in drone-osc/osc_svf.h, which keeps the poles inside the unit
 *  circle across the whole 1 Hz .. 20 kHz range at 48 kHz (at 20 kHz the raw
 *  cc is ~1.0, clamped to 0.92; the -3 dB point lands ~16 kHz, well below
 *  Nyquist). The ladder uses a CONSTANT cc_ (K-rate) so it remains a
 *  time-invariant system with a well-defined DC-tracking property: the LP
 *  integrator (pos) tracks the DC of the input, and the HP tap (in - pos)
 *  passes the AC without latching a DC offset. The sweep is applied as a
 *  post-ladder gain factor (cc_w / cc_, clamped to [0.1, 1.0]) on the HP
 *  output, which emulates the cutoff sweep without perturbing the
 *  integrator states (which would break the DC-tracking property and kill
 *  the HP, the "sound dies after dragging sweep amt" bug).
 */

#include "utils/float_math.h"

class HwFilter
{
public:
  // Filter types. THRU = dry wire (the block is out of the path).
  enum Type
  {
    THRU = 0,
    HIGH4,
    HIGH2,
    BAND4,
    BAND2,
    LOW4,
    LOW2,
    TYPE_COUNT
  };

  // Parameter ids for set/get (the UI drives them one by one).
  enum
  {
    PARAM_CUTOFF = 0,
    PARAM_RES,
    PARAM_SWEEP_F,
    PARAM_SWEEP_AMT,
    PARAM_COUNT
  };

  // ---- Coefficient maps -----------------------------------------------------

  // Cutoff range (Hz): log-mapped. 1 Hz keeps the low end effectively
  // open; 20 kHz is the full-open point (below the 24 kHz Nyquist).
  static constexpr float kCutoffMinHz = 1.0f;
  static constexpr float kCutoffMaxHz = 20000.0f;

  // Resonance: the RES knob maps linearly onto [kQMin, kQMax]. 0.5 is a
  // gently over-damped filter, 2.5 a pronounced peak. Q stays below 3 to
  // avoid the ladder's high-Q instability near the top of the range.
  static constexpr float kQMin = 0.5f;
  static constexpr float kQMax = 2.5f;

  // Per-sample coefficient clamps (see the stability note in the header).
  static constexpr float kCcMin = 0.001f;
  static constexpr float kCcMax = 0.92f;
  static constexpr float kAeMin = 0.18f;
  static constexpr float kAeMax = 1.0f;

  // Sweep F range (Hz): the sweep-rate knob 0..30 Hz. 0 = sweep off (the
  // phase never advances, so the cutoff sits still).
  static constexpr float kSweepFreqMaxHz = 30.0f;

  // Sweep amount: the -100..+100 knob sets the deviation depth in octaves.
  // +/-kSweepMaxOctaves around the cutoff knob value (multiplicative,
  // symmetric in dB, never crosses zero).
  static constexpr float kSweepMaxOctaves = 1.0f;

  // Cutoff Hz from a knob value (0..1): log-mapped (same shape as
  // Svf::cutoffHz in drone-osc/osc_svf.h, with a 1 Hz .. 20 kHz range).
  static inline float cutoffHz(float knob)
  {
    return kCutoffMinHz * fasterpowf(kCutoffMaxHz / kCutoffMinHz, knob);
  }

  // Resonance Q from a knob value (0..1): linear [kQMin, kQMax].
  static inline float quality(float knob)
  {
    return kQMin + knob * (kQMax - kQMin);
  }

  // Per-sample cutoff coefficient cc for a given frequency: cc = 2*sin(w0)
  // with w0 = 2*pi*fc/sr, clamped to (kCcMin, kCcMax]. K-rate only.
  static inline float ccForHz(float fc, float sr)
  {
    const float w0 = 6.283185307179586f * fc / sr;
    float cc = 2.0f * fastsinf(w0);
    if (cc < kCcMin) cc = kCcMin;
    else if (cc > kCcMax) cc = kCcMax;
    return cc;
  }

  // Damping coefficient ae from the resonance Q: ae = 1 - cc/(2*Q), clamped
  // to [kAeMin, kAeMax] so the poles stay inside the unit circle even at
  // high Q near the top of the cutoff range.
  static inline float aeForQ(float q, float cc)
  {
    float ae = 1.0f - cc / (2.0f * q);
    if (ae < kAeMin) ae = kAeMin;
    else if (ae > kAeMax) ae = kAeMax;
    return ae;
  }

  // ---- State ------------------------------------------------------------------

  // One ladder (two-pole state-variable filter, Ladder topology). The math
  // is identical to Svf::State in drone-osc/osc_svf.h (vel / pos integration);
  // the only difference is that the tap (LP/BP/HP) is applied in process()
  // per type rather than a dry/wet crossfade.
  struct Ladder
  {
    float vel = 0.0f;  // band-pass state (velocity)
    float pos = 0.0f;  // low-pass state (position)
    void reset()
    {
      vel = 0.0f;
      pos = 0.0f;
    }
  };

  HwFilter() = default;

  void reset()
  {
    for (int c = 0; c < 2; ++c)
    {
      ladders_[c][0].reset();
      ladders_[c][1].reset();
    }
  }

  // ---- Parameters (main thread writes; K-rate consumed by apply()) -----------

  void setType(int type)
  {
    if (type < 0 || type >= TYPE_COUNT) return;
    type_ = type;
    if (type_ == THRU)
      reset();
    dirty_ = 1;
  }
  int getType() const { return type_; }

  // Set one parameter by id. value ranges:
  //   PARAM_CUTOFF / PARAM_RES / PARAM_SWEEP_AMT : raw 0..1023
  //   PARAM_SWEEP_F                               : 0..3000 (milli-Hz, 0..30 Hz)
  void setParam(int which, int32_t raw)
  {
    switch (which)
    {
    case PARAM_CUTOFF:
      cutoff_ = raw;
      break;
    case PARAM_RES:
      res_ = raw;
      break;
    case PARAM_SWEEP_F:
      sweep_f_ = raw;
      break;
    case PARAM_SWEEP_AMT:
      sweep_amt_ = raw;
      break;
    default:
      return;
    }
    dirty_ = 1;
  }
  int32_t getParam(int which) const
  {
    switch (which)
    {
    case PARAM_CUTOFF: return cutoff_;
    case PARAM_RES: return res_;
    case PARAM_SWEEP_F: return sweep_f_;
    case PARAM_SWEEP_AMT: return sweep_amt_;
    default: return 0;
    }
  }

  // ---- Audio path (worklet thread) --------------------------------------------

  // Apply any changed K-rate coefficients (cutoff / res knobs, type). Called
  // at the top of Chain::process, once per block, on the audio thread. The
  // sweep F and sweep AMT are consumed per-sample below (they are cheap).
  void apply()
  {
    if (!dirty_) return;
    dirty_ = 0;
    const float sr = Processor::getSampleRate();
    const float knob = static_cast<float>(cutoff_) / 1023.0f;
    const float q = quality(static_cast<float>(res_) / 1023.0f);
    cc_ = ccForHz(cutoffHz(knob), sr);
    ae_ = aeForQ(q, cc_);
  }

  // Advance the filter by one block. in / out are interleaved STEREO
  // (in[2i]=L, in[2i+1]=R), as everywhere else in the chain. THRU is a dry
  // wire (out == in, the block is out of the path).
  void process(const float *__restrict in, float *__restrict out, uint32_t frames)
  {
    if (type_ == THRU)
    {
      for (uint32_t i = 0; i < 2 * frames; ++i) out[i] = in[i];
      return;
    }
    apply(); // pick up any K-rate change (type / cutoff / res) on this block

    const int cascade = (type_ == HIGH4 || type_ == BAND4 || type_ == LOW4) ? 2 : 1;
    // Sweep deviation in octaves: raw 0..1023 -> -1..+1, scaled to
    // +/-kSweepMaxOctaves. dev=0 (raw ~511) means no wander.
    const float dev = kSweepMaxOctaves * (static_cast<float>(sweep_amt_) / 511.5f - 1.0f);
    // Per-sample sweep phase step (0 when sweep F = 0: the phase never
    // advances, so the cutoff sits still). One shared phase drives both
    // channels (a single global LFO, as on the hardware).
    const float sweep_freq_hz = static_cast<float>(sweep_f_) / 1000.0f;
    const float phase_step = (sweep_freq_hz > 0.0f)
        ? (6.283185307179586f * sweep_freq_hz / Processor::getSampleRate())
        : 0.0f;
    const float sr = Processor::getSampleRate();
    const float fc_knob = cutoffHz(static_cast<float>(cutoff_) / 1023.0f);

    for (int c = 0; c < 2; ++c)
    {
      for (uint32_t i = 0; i < frames; ++i)
      {
        // Per-sample cutoff coefficient with the sweep:
        //   fc_eff = fc_knob * 2^(dev * sin(phase)), cc_w clamped like cc.
        const float cc_w = (dev != 0.0f)
            ? ccForHz(fc_knob * fasterpowf(2.0f, dev * fastsinf(sweep_phase_)), sr)
            : cc_;
        // Advance the sweep phase (mod 2pi, so it never overflows).
        sweep_phase_ += phase_step;
        if (sweep_phase_ >= 6.283185307179586f)
          sweep_phase_ -= 6.283185307179586f;

        const float x = in[2 * i + c];
        float y = x;
        for (int k = 0; k < cascade; ++k)
        {
          Ladder &ld = ladders_[c][k];
          // Ladder SVF with CONSTANT cc_ (K-rate): vel += cc * (in - pos -
          // ae*vel); pos += cc * vel. The constant cc keeps the ladder a
          // time-invariant system, so its DC-tracking property holds: the LP
          // integrator (pos) tracks the DC of the input, and the HP tap
          // (in - pos) passes the AC without latching a DC offset (the
          // "sound dies after dragging sweep amt" bug).
          const float v = ld.vel + cc_ * (y - ld.pos - ae_ * ld.vel);
          const float p = ld.pos + cc_ * v;
          ld.vel = v;
          ld.pos = p;
          // Cascade: intermediate ladders pass their LP output; the LAST
          // ladder's tap (LP / BP / HP per type) is the filter output.
          y = (k + 1 < cascade) ? p : tap(p, v, x);
        }
        // Post-ladder sweep: the per-sample cc_w modulates the effective
        // cutoff frequency. For the HP tap, a lower cc_w means a lower
        // cutoff, which attenuates the passband. Emulate this with a
        // gain factor = cc_w / cc_ (clamped to [0.1, 1.0] to avoid
        // over-attenuation). This is a first-order approximation of the
        // cutoff sweep that does NOT perturb the ladder's integrator
        // states (which would break the DC-tracking property and kill the
        // HP). For the LP/BP taps, the sweep is not applied (the user
        // expectation is that sweep modulates the HP passband, not the LP
        // stopband or the BP center).
        if ((type_ == HIGH2 || type_ == HIGH4) && dev != 0.0f && cc_ > 0.0f)
        {
          float gain = cc_w / cc_;
          if (gain < 0.1f) gain = 0.1f;
          else if (gain > 1.0f) gain = 1.0f;
          y *= gain;
        }
        out[2 * i + c] = y;
      }
    }
  }

private:
  // The final-stage tap per type. pos = LP state, vel = BP state, in = the
  // original channel sample for the HP tap. For the 4-pole types only the
  // LAST ladder's tap is taken (the intermediate stage contributes its LP
  // as the cascade input) -- the standard way to build a 4-pole from two
  // 2-pole ladders.
  inline float tap(float pos, float vel, float in) const
  {
    switch (type_)
    {
    case LOW2:
    case LOW4:
      return pos; // low-pass: the position state
    case BAND2:
    case BAND4:
      return vel; // band-pass: the velocity state
    case HIGH2:
    case HIGH4:
      return in - pos; // high-pass: input minus the low-pass state
    default:
      return in; // THRU (should not reach here)
    }
  }

  // Per-channel ladder states: [channel][cascade stage]. Stage 1 (index 1)
  // is only used by the 4-pole types.
  Ladder ladders_[2][2];

  // K-rate coefficients (shared across channels; recomputed in apply()).
  float cc_ = 0.0f;
  float ae_ = 1.0f;

  // Sweep phase (radians, 0..2pi). A single global phase drives both channels.
  float sweep_phase_ = 0.0f;

  // Parameter raw values (main thread writes; the worklet reads). Plain
  // aligned 32-bit ints: benign data race, exactly like the FX cache.
  int32_t cutoff_ = 0;        // raw 0..1023 (default 0 = min cutoff)
  int32_t res_ = 0;           // raw 0..1023 (default 0 = min Q)
  int32_t sweep_f_ = 0;       // raw 0..3000 milli-Hz (default 0 = sweep off)
  int32_t sweep_amt_ = 511;   // raw 0..1023 (default 511 ~ 0% = no wander)
  int type_ = THRU;           // filter type (default THRU = dry wire)
  volatile int32_t dirty_ = 1; // apply() pending (set by main-thread setters)
};
