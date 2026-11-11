#pragma once
/*
 *  File: chain.h
 *
 *  Web-simulator-only "chain" processor: the full DRONE audio path
 *  OSC -> MOD -> DEL -> REV -> out, running as a SINGLE wasm audio worklet.
 *
 *  This is a web-simulator-only class. It is NOT part of any ARM unit build:
 *  on the NTS-1 the four modules are loaded as separate units and the
 *  platform routes the audio (OSC -> MOD -> DEL -> REV -> output) itself.
 *  In the browser there is no platform, so the simulator links the four
 *  existing Processor classes into ONE worklet and runs the same cascade
 *  inside its ProcessAudio callback. The FX DSP is currently a pass-through
 *  (see drone-modfx/modfx.h, drone-delfx/delay.h, drone-revfx/reverb.h); the
 *  chain therefore reads as a clean OSC until the FX stages get real DSP.
 *
 *  The four stages are the actual production classes (Osc / Modfx / Delay /
 *  Reverb) so the simulator always reflects the real unit behaviour. Each
 *  stage owns its own parameters: OSC through the 44 edit-parameter
 *  multiplexer (driven by the UI via Module.setEditParam), the three FX
 *  through TIME / DEPTH (driven by the UI via Module.setFxParam).
 *
 *  Signal geometry: Osc outputs MONO (1 channel, 128 samples). Every FX
 *  stage is stereo (2 channels, interleaved). The chain therefore:
 *    1. renders OSC to a mono scratch buffer,
 *    2. upmixes mono -> stereo (duplicate L/R) for the first FX stage,
 *    3. runs Mod -> Del -> Rev in series (each in-place between two
 *       interleaved stereo scratch buffers),
 *    4. writes the final stereo (interleaved L/R) to the 2-channel worklet
 *       output. The web simulator's worklet node has outputChannelCounts={2},
 *       so the browser delivers true stereo to the speakers. (On the NTS-1
 *       the four modules are separate units and the platform routes the audio
 *       itself; the stereo output is a web-simulator-only convenience.)
 *
 *  RAM: Osc needs none (getBufferSize()==0). The FX stages claim buffers
 *  (Mod 256 KB, Del 1 MB, Rev 1 MB in float samples). Chain::init allocates
 *  ONE contiguous float buffer (sum of all getBufferSize()) and slices it,
 *  mirroring what the platform's sdram_alloc would do per unit. In the web
 *  simulator this buffer lives in the wasm linear heap (a few MB, no problem).
 */

#include "processor.h"

// The four production DSP classes. All are header-only (their audio path and
// parameter handling live in inline methods / out-of-line headers they
// #include), so they all compile into this single wasm object.
#include "osc.h"     // ../drone-osc/osc.h      (Osc)
#include "modfx.h"   // ../drone-modfx/modfx.h  (Modfx)
#include "delay.h"   // ../drone-delfx/delay.h  (Delay)
#include "reverb.h"  // ../drone-revfx/reverb.h (Reverb)

class Chain : public Processor
{
public:
  // ---- Web-sim-only FX stage / param ids -------------------------------------
  // The UI drives each FX through setFxParam(stage, which, raw); these map 1:1
  // onto each FX class's TIME / DEPTH parameter ids (both TIME==0, DEPTH==1 in
  // Modfx, Delay and Reverb). Kept here so the UI binding and the worklet
  // agree on a single enumeration.
  enum
  {
    FX_MOD = 0,
    FX_DEL,
    FX_REV,
    FX_STAGE_COUNT
  };
  // MOD is a full 7-parameter stage (TIME / DEPTH / DRIFT / ENSE / TREMO /
  // FLNG / TAPE). DEL and REV still have their own parameter sets (DELAY /
  // FDBK / ... / DENS / ...). We use the MOD stage's count (7) as the per-stage
  // stride so setFxParam/applyFxParams work for all three; DEL/REV simply
  // ignore the higher indices (their setParameter default-cases them).
  // FX_PARAM_COUNT is the MOD count = 7.
  enum
  {
    FX_TIME = 0,
    FX_DEPTH,
    FX_DRIFT,
    FX_ENSE,
    FX_TREMO,
    FX_FLNG,
    FX_TAPE,
    FX_PARAM_COUNT // = 7 (the MOD stage's parameter count)
  };

  // Total dynamic RAM the chain needs = the sum of every stage's buffer.
  uint32_t getBufferSize() const override final
  {
    return osc_.getBufferSize() + mod_.getBufferSize() + del_.getBufferSize() + rev_.getBufferSize();
  }

  // Allocate `allocated_buffer` (getBufferSize() floats, owned by the caller --
  // the wasm.cc `ram` vector) across the three FX stages. Osc needs none.
  void init(float *allocated_buffer) override final
  {
    float *p = allocated_buffer;
    // Osc needs no buffer.
    if (mod_.getBufferSize() > 0)
    {
      mod_.init(p);
      p += mod_.getBufferSize();
    }
    else
    {
      mod_.init(nullptr);
    }
    if (del_.getBufferSize() > 0)
    {
      del_.init(p);
      p += del_.getBufferSize();
    }
    else
    {
      del_.init(nullptr);
    }
    if (rev_.getBufferSize() > 0)
    {
      rev_.init(p);
      p += rev_.getBufferSize();
    }
    else
    {
      rev_.init(nullptr);
    }
    osc_.init(nullptr);
  }

  void teardown() override final
  {
    osc_.teardown();
    mod_.teardown();
    del_.teardown();
    rev_.teardown();
  }

  // The full cascade, one audio block (frames = WEB_AUDIO_FRAME_SIZE).
  // `in` is unused (the OSC is a generator and ignores its input); `out` is
  // the STEREO output (2*frames interleaved samples: out[2i]=L, out[2i+1]=R).
  // The hardware OSC is mono, but the FX stages (MOD/DEL/REV) are stereo, so
  // the chain upmixes OSC to L/R at the front and keeps it stereo all the way
  // to the output. The web simulator's worklet node has 2 output channels;
  // the browser's channel-routing delivers true stereo to the speakers.
  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final
  {
    (void)in;

    // Apply any FX TIME/DEPTH the UI changed (main thread wrote the shared
    // cache; the worklet thread consumes it here, once per block).
    applyFxParams();

    // 1. OSC -> mono.
    osc_.process(nullptr, osc_mono_, frames);
    
    // Diagnostic: track the peak of the OSC output
    float osc_peak = 0.f;
    for (uint32_t i = 0; i < frames; ++i) {
      const float abs = (osc_mono_[i] < 0.f) ? -osc_mono_[i] : osc_mono_[i];
      if (abs > osc_peak) osc_peak = abs;
    }
    osc_output_peak_ = osc_peak;

    // 2. mono -> stereo (duplicate), the input to the first FX stage.
    for (uint32_t i = 0; i < frames; ++i)
    {
      buf_a_[2 * i] = osc_mono_[i];
      buf_a_[2 * i + 1] = osc_mono_[i];
    }

    // 3. MOD -> DEL -> REV, each in-place between two interleaved stereo
    //    scratch buffers (in/out ping-pong). Each stage can be BYPASSED
    //    (sim_stage_bypass_[stage] == 1): when bypassed the buffer is passed
    //    straight through untouched, so the user can A/B any combination
    //    OSC -> [MOD] -> [DEL] -> [REV] and localize a clipping stage.
    const float *fx_in = buf_a_;
    float *fx_out = buf_b_;
    if (!sim_stage_bypass_[FX_MOD])
    {
      mod_.process(fx_in, fx_out, frames);
      const float *t = fx_out; fx_out = const_cast<float *>(fx_in); fx_in = t;
    }
    if (!sim_stage_bypass_[FX_DEL])
    {
      del_.process(fx_in, fx_out, frames);
      const float *t = fx_out; fx_out = const_cast<float *>(fx_in); fx_in = t;
    }
    if (!sim_stage_bypass_[FX_REV])
    {
      rev_.process(fx_in, fx_out, frames);
      const float *t = fx_out; fx_out = const_cast<float *>(fx_in); fx_in = t;
    }

    // 4. Write the stereo output (interleaved L/R). If every stage is
    //    bypassed, fx_in is still buf_a_ where L==R==osc (a clean dry
    //    mono reference duplicated to both channels).
    for (uint32_t i = 0; i < frames; ++i)
    {
      out[2 * i]     = fx_in[2 * i];
      out[2 * i + 1] = fx_in[2 * i + 1];
    }
  }

  // ---- Note / pitch / tempo routing (forwarded to the OSC stage) -------------
  void setPitch(float w0) { osc_.setPitch(w0); }
  void noteOn(uint8_t note, uint8_t velo) { osc_.noteOn(note, velo); }
  void noteOff(uint8_t note) { osc_.noteOff(note); }
  void allNoteOff() { osc_.allNoteOff(); }
  // Simulator-only LFO (mirrors the hardware OSC-button A/B knobs; the OSC
  // synthesizes it internally). Forwarded so the chain's UI can drive it.
  void oscLfoFreq(float freq) { osc_.setLfoFreq(freq); }
  void oscLfoDepth(float depth) { osc_.setLfoDepth(depth); }
  void setTempo(float bpm) override
  {
    osc_.setTempo(bpm);
    // The FX stages also take tempo (BPM-locked delay times, etc.). Forward it
    // so a real BPM-based FX can later use it; the current pass-through DSP
    // ignores it.
    mod_.setTempo(bpm);
    del_.setTempo(bpm);
    rev_.setTempo(bpm);
  }
  void tempo4ppqnTick(uint32_t counter) override
  {
    osc_.tempo4ppqnTick(counter);
  }

  // ---- OSC edit-parameter accessors (for the 44-parameter UI) ----------------
  uint8_t getSelect() const { return osc_.getSelect(); }
  int32_t getEditMorphValue(uint8_t idx) const { return osc_.getEditMorphValue(idx); }
  bool isMorphActive() const { return osc_.isMorphActive(); }
  void setParameter(uint8_t index, int32_t value) { osc_.setParameter(index, value); }
  const char *getParameterStrValue(uint8_t id, int32_t value) const
  {
    return osc_.getParameterStrValue(id, value);
  }
  void simRndRequest(uint8_t mode) { osc_.simRndRequest(mode); }
  void stableSync() { osc_.stableSync(); }
  uint8_t getRndMode() const { return osc_.getRndMode(); }
  void setRndMode(uint8_t mode) { osc_.setRndMode(mode); }
  uint8_t getRndRun() const { return osc_.getRndRun(); }
  void setRndRun(uint8_t v) { osc_.setRndRun(v); }

  // ---- FX parameter accessors ------------------------------------------------
  // The UI (main thread) writes an FX TIME/DEPTH value; the worklet applies it
  // each block (applyFxParams, called from process()). Main thread and worklet
  // thread share one linear memory (SHARED_MEMORY=1), so a plain 32-bit aligned
  // int write is visible to the worklet on its next block (a benign data race,
  // exactly like Osc's sim_rnd_request_ / sim_edit_request_).
  //
  // setFxParam is called from the main thread: store the raw value in the
  // shared cache so (a) the worklet applies it to the live DSP each block and
  // (b) getFxParam can read it back for UI init.
  void setFxParam(int stage, int which, int32_t raw)
  {
    if (stage < 0 || stage >= FX_STAGE_COUNT || which < 0 || which >= FX_PARAM_COUNT)
      return;
    sim_fx_cache_[stage * FX_PARAM_COUNT + which] = raw;
    sim_fx_valid_ = 1; // arm: the worklet will (re-)apply on the next block
  }

  int32_t getFxParam(int stage, int which) const
  {
    if (stage < 0 || stage >= FX_STAGE_COUNT || which < 0 || which >= FX_PARAM_COUNT)
      return 0;
    return sim_fx_cache_[stage * FX_PARAM_COUNT + which];
  }

  // ---- Per-stage BYPASS (web-sim diagnostic A/B) -----------------------------
  // A per-stage on/off switch so the user can hear any combination of
  // OSC -> [MOD] -> [DEL] -> [REV] and localize which stage is clipping.
  //   bypass == 0 (default) -> the stage is IN the path.
  //   bypass == 1           -> the stage is skipped (dry wire through).
  // Main thread writes (UI toggle), worklet reads in process(). Plain aligned
  // 32-bit: benign data race, exactly like sim_fx_valid_.
  void setStageBypass(int stage, int on)
  {
    if (stage < 0 || stage >= FX_STAGE_COUNT)
      return;
    sim_stage_bypass_[stage] = on ? 1 : 0;
  }

  int32_t getStageBypass(int stage) const
  {
    if (stage < 0 || stage >= FX_STAGE_COUNT)
      return 0;
    return sim_stage_bypass_[stage];
  }

  // Diagnostic: read MOD input/output peaks (for debugging gain issues).
  float getModInputPeak() const { return mod_.mod_input_peak_; }
  float getModOutputPeak() const { return mod_.mod_output_peak_; }

  // Diagnostic: read the OSC output peak (to verify the oscillator is running).
  volatile float osc_output_peak_ = 0.f;
  float getOscOutputPeak() const { return osc_output_peak_; }

  // Called from the worklet thread once per block (top of process()): apply the
  // cached FX values to the live DSP. `w` (FX_TIME=0 / FX_DEPTH=1) maps 1:1 onto
  // each FX class's TIME / DEPTH parameter ids.
  void applyFxParams()
  {
    if (!sim_fx_valid_)
      return;
    for (int s = 0; s < FX_STAGE_COUNT; ++s)
    {
      for (int w = 0; w < FX_PARAM_COUNT; ++w)
      {
        int32_t raw = sim_fx_cache_[s * FX_PARAM_COUNT + w];
        switch (s)
        {
        case FX_MOD:
          mod_.setParameter(static_cast<uint8_t>(w), raw);
          break;
        case FX_DEL:
          del_.setParameter(static_cast<uint8_t>(w), raw);
          break;
        case FX_REV:
          rev_.setParameter(static_cast<uint8_t>(w), raw);
          break;
        default:
          break;
        }
      }
    }
  }

private:
  static constexpr uint32_t kFrames = 128U; // WEB_AUDIO_FRAME_SIZE (the worklet block)

  Osc osc_;    // generator (mono out, no buffer)
  Modfx mod_;  // FX stage 1 (stereo, 256 KB)
  Delay del_;  // FX stage 2 (stereo, 1 MB)
  Reverb rev_; // FX stage 3 (stereo, 1 MB)

  // Scratch buffers: one mono (OSC out) + two interleaved stereo (FX ping-pong).
  // Sized for kFrames (128) samples; the worklet always calls with 128.
  // Zero-initialized to avoid garbage on first block (causes DC offset / noise).
  float osc_mono_[kFrames] = {};
  float buf_a_[2 * kFrames] = {};
  float buf_b_[2 * kFrames] = {};

  // Shared FX parameter cache (main thread writes, worklet reads/applies).
  // Plain aligned 32-bit ints: benign data race, inaudible.
  // Zero-initialized so getFxParam returns 0 (not garbage) before first setFxParam.
  int32_t sim_fx_cache_[FX_STAGE_COUNT * FX_PARAM_COUNT] = {};
  volatile int32_t sim_fx_valid_ = 0;

  // Per-stage bypass flags (web-sim diagnostic A/B). 0 = stage IN path
  // (default), 1 = stage skipped. See setStageBypass/getStageBypass.
  volatile int32_t sim_stage_bypass_[FX_STAGE_COUNT] = {0, 0, 0};
};

