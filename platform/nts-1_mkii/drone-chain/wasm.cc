// reference: https://emscripten.org/docs/api_reference/wasm_audio_worklets.html#wasm-audio-worklets
// example code: https://github.com/emscripten-core/emscripten/tree/main/test/webaudio

// Web-simulator "drone-chain": the full DRONE audio path OSC -> MOD -> DEL ->
// REV -> out, running as a SINGLE wasm audio worklet (see chain.h). The DSP is
// the Chain class (drone-chain/chain.h); this file wires it into the emscripten
// audio-worklet engine and exposes the UI bindings (the 44 OSC edit parameters
// plus the three FX's TIME/DEPTH).
//
// Modelled on drone-osc/wasm.cc: the same audio-thread / worklet bootstrap, the
// same Park-Miller-Carta noise stubs (Osc needs osc_white/osc_rand), and the
// same "main thread and worklet thread share one linear memory" delivery model
// for parameters (setEditParam / setFxParam write, the worklet consumes).

#include <emscripten/bind.h>
#include <emscripten/webaudio.h>
#include <emscripten/em_math.h>
#include <string>
#include <vector>
#include <array>
#include <cmath>
#include <cstdio>
#include <cassert>
using namespace emscripten;
#include "chain.h"

// this needs to be big enough for the stereo output, inputs, params and the worker stack
uint8_t audioThreadStack[4096];

constexpr int SAMPLE_RATE = 48000;
constexpr int WEB_AUDIO_FRAME_SIZE = 128;
std::vector<float> ram;
std::array<float, WEB_AUDIO_FRAME_SIZE> interleavedOut;

Chain processor; // the chained OSC->MOD->DEL->REV dsp instance
extern const unit_header_t unit_header;

static float BPM_WASM = 120.f;

void fx_set_bpm(float bpm)
{
  BPM_WASM = bpm;
  processor.setTempo(bpm);
}

uint16_t fx_get_bpm(void)
{
  return static_cast<int>(BPM_WASM * 10.f);
}

float fx_get_bpmf(void)
{
  return BPM_WASM;
}

// ---- Web-sim stubs for the platform noise API --------------------------------
// On ARM, osc_white() / osc_rand() are provided by the runtime (osc_api.h
// extern). In the web simulator there is no runtime, so we provide lightweight
// noise sources based on the Park-Miller-Carta LCG (matching
// websim/dsp/osc_api.cpp), exactly as drone-osc/wasm.cc does, because the OSC
// stage (Osc) consumes them for its random-walk detune and Rnd re-rolls.
namespace {
uint32_t s_pmc_state = 83647U; // Park-Miller-Carta seed (matches websim DEFAULT_SEED)
inline uint32_t pmc_rand(uint32_t s)
{
  uint32_t lo = 16807u * (s & 0xFFFFu);
  const uint32_t hi = 16807u * (s >> 16);
  lo += (hi & 0x7FFFu) << 16;
  lo += hi >> 15;
  return (lo & 0x7FFFFFFFu) + (lo >> 31);
}
} // namespace

extern "C" float osc_white(void)
{
  // Uniform noise in [-1, 1] from a Park-Miller-Carta sample.
  s_pmc_state = pmc_rand(s_pmc_state);
  return static_cast<float>(s_pmc_state) * (2.0f / 2147483647.0f) - 1.0f;
}

extern "C" uint32_t osc_rand(void)
{
  // Raw Park-Miller-Carta state in [1, 0x7FFFFFFF] (the firmware's osc_rand()
  // has the same range). This is what Osc::rnd_int() consumes for the Rnd
  // re-roll, so the simulator re-rolls bit-identical values to the device.
  s_pmc_state = pmc_rand(s_pmc_state);
  return s_pmc_state;
}

struct AudioWorkletParameter
{
  int min;
  int max;
  int center;
  int init;
  uint8_t type;
  std::string name;
};

std::string getParameterValueString(int index, int value)
{
  const unit_param_t &p = unit_header.params[index];

  // The OSC stage implements getParameterStrValue() for the PARAM/VALUE slots;
  // route through the chain (which forwards to Osc) first so the simulator UI
  // stays in lockstep with the hardware display, then fall through to the
  // suffix-based formatter for any other slot.
  const char *s = processor.getParameterStrValue(static_cast<uint8_t>(index), value);
  if (s != nullptr)
    return std::string(s);

  std::string suffix;

  switch (p.type)
  {
  case k_unit_param_type_none:
    break;
  case k_unit_param_type_percent:
    suffix = "%";
    break;
  case k_unit_param_type_db:
    suffix = " dB";
    break;
  case k_unit_param_type_cents:
    suffix = " cents";
    break;
  case k_unit_param_type_semi:
    suffix = " semitones";
    break;
  case k_unit_param_type_oct:
    suffix = " octaves";
    break;
  case k_unit_param_type_hertz:
    suffix = " Hz";
    break;
  case k_unit_param_type_khertz:
    suffix = " kHz";
    break;
  case k_unit_param_type_bpm:
    suffix = " bpm";
    break;
  case k_unit_param_type_msec:
    suffix = " ms";
    break;
  case k_unit_param_type_sec:
    suffix = " s";
    break;
  case k_unit_param_type_enum:
    break;
  case k_unit_param_type_strings:
    return processor.getParameterStrValue(index, value);
    break;
  case k_unit_param_type_drywet:
    suffix = "%";
    break;
  case k_unit_param_type_pan:
  case k_unit_param_type_spread:

    if (value < 0)
    {
      suffix = "L";
    }
    else if (value > 0)
    {
      suffix = "R";
    }
    else if (value == p.center)
    {
      return "CNTR";
    }
    break;

  case k_unit_param_type_onoff:
    if (value == 0)
    {
      return "OFF";
    }
    else
    {
      return "ON";
    }
    break;
  case k_unit_param_type_midi_note:
    // todo
  default:
    return "unimplemented";
    break;
  };

  std::string numerical;
  if (p.frac_mode == k_unit_param_frac_mode_fixed)
  {
    numerical = std::to_string(value / static_cast<double>(1 << p.frac));
  }
  else
  {
    numerical = std::to_string(value / std::pow(10.0, p.frac));
  }
  numerical.erase(numerical.find_last_not_of('0') + 1);
  if (!numerical.empty() && numerical.back() == '.')
  {
    numerical.pop_back();
  }

  return numerical + suffix;
}

std::vector<AudioWorkletParameter> getValidParameters()
{
  std::vector<AudioWorkletParameter> result;
  for (int i = 0; i < unit_header.num_params; ++i)
  {
    const unit_param_t &p = unit_header.params[i];
    result.push_back({p.min,
                      p.max,
                      p.center,
                      p.init,
                      p.type,
                      std::string(p.name)});
  }
  return result;
}

// Cached native parameter ranges, used by ProcessAudio to map the normalized
// Web Audio Param value [0,1] back onto each unit parameter's integer range.
static const std::vector<AudioWorkletParameter> &valid_parameters_ref()
{
  static const std::vector<AudioWorkletParameter> params = getValidParameters();
  return params;
}

void setOscPitch(float f0)
{
  processor.setPitch(f0 / static_cast<float>(SAMPLE_RATE));
}

void noteOn(uint8_t note, uint8_t velocity)
{
  processor.noteOn(note, velocity);
}

// note off velocity is not supported by logue-sdk
void noteOff(uint8_t note)
{
  processor.noteOff(note);
}

// Simulator-only: LFO settings (mirrors the hardware OSC-button A/B knobs).
// The processor synthesizes the LFO at audio rate internally; these just set
// its frequency (Hz) and depth (0..1). On hardware the platform delivers the
// LFO via setShapeLfo() instead, so these are no-ops there (compiled out).
void setLfoFreq(float freq)
{
  processor.oscLfoFreq(freq);
}

void setLfoDepth(float depth)
{
  processor.oscLfoDepth(depth);
}

// ---- FX parameter bindings (MOD / DEL / REV, each TIME + DEPTH) --------------
// stage: 0=MOD, 1=DEL, 2=REV (Chain::FX_MOD / FX_DEL / FX_REV)
// which: 0=TIME, 1=DEPTH (Chain::FX_TIME / FX_DEPTH)
// raw:   0..1023 (10-bit knob value, like the FX header TIME/DEPTH slots)
//
// The UI (main thread) calls setFxParam; the value is stored in the chain's
// shared sim_fx_cache_ and applied to the live DSP by the worklet thread each
// block (Chain::applyFxParams). Main thread and worklet share one linear
// memory (SHARED_MEMORY=1), so the write is visible on the next block.
extern "C" void setFxParam(int stage, int which, int raw)
{
  processor.setFxParam(stage, which, raw);
}

extern "C" int getFxParam(int stage, int which)
{
  return static_cast<int>(processor.getFxParam(stage, which));
}

// ---- Per-stage BYPASS (diagnostic A/B: OSC -> [MOD] -> [DEL] -> [REV]) -------
// stage: 0=MOD, 1=DEL, 2=REV (Chain::FX_MOD / FX_DEL / FX_REV).
// on:    0 = stage IN the path (default), 1 = stage skipped (dry wire).
// Stored in the chain's shared sim_stage_bypass_[] and consumed by the worklet
// thread in Chain::process each block. Same benign shared-memory race as
// setFxParam.
extern "C" void setStageBypass(int stage, int on)
{
  processor.setStageBypass(stage, on);
}

extern "C" int getStageBypass(int stage)
{
  return static_cast<int>(processor.getStageBypass(stage));
}

// ---- OSC edit-parameter accessors (the 44-parameter UI) ----------------------
// Mirror drone-osc/wasm.cc: the main-thread `processor` and the worklet-thread
// `processor` are the SAME object (shared linear memory), so writing through
// setEditParam is visible to the worklet on its next block. getSelectParam /
// getEditParamValue read the main-thread state for UI init.
extern "C" int getSelectParam()
{
  return static_cast<int>(processor.getSelect());
}

extern "C" int getEditParamValue(int idx)
{
  // While a Rnd re-roll morph is in flight, return the CURRENT interpolated raw
  // value (getEditMorphValue) so the sim's sliders travel toward the re-roll
  // target instead of jumping to it on the first poll.
  return static_cast<int>(processor.getEditMorphValue(static_cast<uint8_t>(idx)));
}

// 1 while a Rnd re-roll morph is in flight, 0 otherwise. The sim's post-fire
// slider-sync poll uses this to know when to STOP.
extern "C" int getMorphActive()
{
  return processor.isMorphActive() ? 1 : 0;
}

// Write one edit-parameter (index 0..43, raw 0..1023) to the multiplexer:
// select the parameter, then write its value (the hardware's two-step
// unit_set_param_value).
extern "C" void setEditParam(int idx, int raw)
{
  processor.setParameter(0, static_cast<int32_t>(idx)); // PARAM_SEL
  processor.setParameter(1, raw);                       // PARAM_VAL
}

// Fire the random re-roll on the audio-worklet thread (mirrors the hardware's
// "Rnd" trigger slot, but NOT debounced -- a single click is a single event).
extern "C" void rndTrigger()
{
  processor.simRndRequest(static_cast<uint8_t>(processor.getRndMode()));
}

extern "C" int getRndMode()
{
  return static_cast<int>(processor.getRndMode());
}

extern "C" void setRndMode(int mode)
{
  processor.setRndMode(static_cast<uint8_t>(mode));
}

extern "C" int getRndRun()
{
  return static_cast<int>(processor.getRndRun());
}

extern "C" void setRndRun(int v)
{
  processor.setRndRun(static_cast<uint8_t>(v));
}

// Re-copy the live edit parameters into the ORBI "stable state" snapshot.
extern "C" void stableSync()
{
  processor.stableSync();
}

// ---- embind: UI-facing module ------------------------------------------------
// bind unit parameters
EMSCRIPTEN_BINDINGS(my_module)
{
  value_object<AudioWorkletParameter>("AudioWorkletParameter")
      .field("min", &AudioWorkletParameter::min)
      .field("max", &AudioWorkletParameter::max)
      .field("center", &AudioWorkletParameter::center)
      .field("init", &AudioWorkletParameter::init)
      .field("type", &AudioWorkletParameter::type)
      .field("name", &AudioWorkletParameter::name);

  register_vector<AudioWorkletParameter>("ParameterList");

  function("getValidParameters", &getValidParameters);

  function("getParameterValueString", &getParameterValueString);

  function("fx_set_bpm", &fx_set_bpm);

  function("setOscPitch", &setOscPitch);

  function("noteOn", &noteOn);

  function("noteOff", &noteOff);

  function("setLfoFreq", &setLfoFreq);

  function("setLfoDepth", &setLfoDepth);
}

EMSCRIPTEN_BINDINGS(drone_ui)
{
  emscripten::function("getSelectParam", &getSelectParam);
  emscripten::function("getEditParamValue", &getEditParamValue);
  emscripten::function("setEditParam", &setEditParam);
  emscripten::function("rndTrigger", &rndTrigger);
  emscripten::function("getRndMode", &getRndMode);
  emscripten::function("setRndMode", &setRndMode);
  emscripten::function("getRndRun", &getRndRun);
  emscripten::function("setRndRun", &setRndRun);
  emscripten::function("stableSync", &stableSync);
  emscripten::function("getMorphActive", &getMorphActive);

  // FX parameters (MOD / DEL / REV x TIME / DEPTH).
  emscripten::function("setFxParam", &setFxParam);
  emscripten::function("getFxParam", &getFxParam);

  // Per-stage bypass (diagnostic A/B). stage: 0=MOD, 1=DEL, 2=REV.
  emscripten::function("setStageBypass", &setStageBypass);
  emscripten::function("getStageBypass", &getStageBypass);
}

// ---- Audio worklet (runs on the dedicated audio thread) ----------------------
// No audio input (the OSC is a generator), one MONO output (matching the
// hardware OSC's 1-channel output). The full OSC->MOD->DEL->REV cascade runs
// inside Chain::process, which averages stereo back to mono at the end.
bool ProcessAudio(int numInputs, const AudioSampleFrame *inputs,
                  int numOutputs, AudioSampleFrame *outputs,
                  int numParams, const AudioParamFrame *params,
                  void *userData)
{
  (void)inputs;
  (void)numParams;
  (void)params;
  (void)userData;
  assert(numInputs == 0);
  assert(numOutputs == 1);
  assert(outputs->numberOfChannels == 1);
  assert(outputs->samplesPerChannel == WEB_AUDIO_FRAME_SIZE);
  auto &output = outputs[0];

  // The OSC edit parameters are driven through the direct edit-parameter
  // channel (setEditParam, consumed by the worklet's shared processor), and the
  // FX parameters through setFxParam (applied by Chain::applyFxParams at the
  // top of process()). The WebAudioParam slots are kept in the descriptor only
  // so the node has the same shape the hardware uses; their values are ignored.

  // emscripten_log(EM_LOG_CONSOLE, "bpm=%d", fx_get_bpm());
  processor.process(nullptr, interleavedOut.data(), WEB_AUDIO_FRAME_SIZE);

  // de-interleave output buffer (mono)
  for (int i = 0; i < WEB_AUDIO_FRAME_SIZE; ++i)
  {
    output.data[i] = interleavedOut[i];
  }
  return true; // Keep the graph output going
}

void AudioWorkletProcessorCreated(EMSCRIPTEN_WEBAUDIO_T audioContext, bool success, void *userData)
{
  if (!success)
    return; // Check browser console in a debug build for detailed errors

  ram.resize(processor.getBufferSize());
  processor.init(ram.data());

  // no input, single mono output
  int outputChannelCounts[1] = {1};
  EmscriptenAudioWorkletNodeCreateOptions options = {
      .numberOfInputs = 0,
      .numberOfOutputs = 1,
      .outputChannelCounts = outputChannelCounts};

  EMSCRIPTEN_AUDIO_WORKLET_NODE_T wasmAudioWorklet = emscripten_create_wasm_audio_worklet_node(audioContext,
                                                                                               "logue-chain", &options, &ProcessAudio, 0);

  EM_ASM({ setupWebAudioAndUI(emscriptenGetAudioObject($0), emscriptenGetAudioObject($1)); }, audioContext, wasmAudioWorklet);
}

void AudioThreadInitialized(EMSCRIPTEN_WEBAUDIO_T audioContext, bool success, void *userData)
{
  if (!success)
    return; // Check browser console in a debug build for detailed errors

  auto valid_parameters = getValidParameters();

  WebAudioParamDescriptor params[valid_parameters.size()];
  for (int i = 0; i < valid_parameters.size(); ++i)
  {
    params[i].automationRate = WEBAUDIO_PARAM_K_RATE;
    // Expose each parameter over its native integer range [min,max]; the UI
    // slider uses param.min/param.max directly and writes values in that range.
    params[i].defaultValue = static_cast<float>(valid_parameters[i].init);
    params[i].minValue = static_cast<float>(valid_parameters[i].min);
    params[i].maxValue = static_cast<float>(valid_parameters[i].max);
  }

  WebAudioWorkletProcessorCreateOptions opts = {
      .name = "logue-chain",
      .numAudioParams = static_cast<int>(valid_parameters.size()),
      .audioParamDescriptors = params};

  emscripten_create_wasm_audio_worklet_processor_async(audioContext, &opts, &AudioWorkletProcessorCreated, 0);
}

int main()
{
  EmscriptenWebAudioCreateAttributes attrs = {
      .latencyHint = "interactive",
      .sampleRate = SAMPLE_RATE};

  EMSCRIPTEN_WEBAUDIO_T context = emscripten_create_audio_context(&attrs);

  int sample_rate = emscripten_audio_context_sample_rate(context);
  int frame_size = emscripten_audio_context_quantum_size(context);
  printf("Sample rate: %d\n", sample_rate);
  printf("Frame size: %d\n", frame_size);

  emscripten_start_wasm_audio_worklet_thread_async(context, audioThreadStack, sizeof(audioThreadStack),
                                                   &AudioThreadInitialized, 0);

  emscripten_exit_with_live_runtime();
}

