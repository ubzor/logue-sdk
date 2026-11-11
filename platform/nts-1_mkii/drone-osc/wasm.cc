// reference: https://emscripten.org/docs/api_reference/wasm_audio_worklets.html#wasm-audio-worklets
// example code: https://github.com/emscripten-core/emscripten/tree/main/test/webaudio

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
#include "osc.h"

// this needs to be big enough for the stereo output, inputs, params and the worker stack
uint8_t audioThreadStack[4096];

constexpr int SAMPLE_RATE = 48000;
constexpr int WEB_AUDIO_FRAME_SIZE = 128;
std::vector<float> ram;
std::array<float, WEB_AUDIO_FRAME_SIZE> interleavedOut;

Osc processor; // dsp processor instance
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
// websim/dsp/osc_api.cpp). osc_white() maps the PMC state to a uniform float
// in [-1, 1]; osc_rand() returns the raw PMC state so rnd_int() (see
// osc_randomize.h) gets the full 31-bit value on both device and sim.
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
  // (The ARM runtime uses a Box-Muller Gaussian; for random-walk detune
  //  modulation the difference between uniform and Gaussian is inaudible.)
  // Note: the LCG state s is in [1, 0x7FFFFFFF], so divide by 0x7FFFFFFF
  // (not 0xFFFFFFFF) to center the distribution on zero.
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

  // The Osc processor implements getParameterStrValue() for the four
  // type_strings header slots (PARAM in slots 0/2, VALUE in slots 1/3).
  // Slots 2/3 are full duplicates of 0/1 (see header.c); the processor
  // routes all four to the same underlying state. Every other index
  // returns "" and we fall through to the suffix-based formatter below.
  // Routing every index through the processor first keeps the simulator UI
  // in lockstep with the hardware display.
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
  // printf("Note On: %d, velocity %d\n", note, velocity);
}

// note off velocity is not supported by logue-sdk
void noteOff(uint8_t note)
{
  processor.noteOff(note);
  // printf("Note Off: %d\n", note);
}

// Simulator-only: LFO settings (mirrors the hardware OSC-button A/B knobs).
// The processor synthesizes the LFO at audio rate internally; these just set
// its frequency (Hz) and depth (0..1). On hardware the platform delivers the
// LFO via setShapeLfo() instead, so these are no-ops there (compiled out).
void setLfoFreq(float freq)
{
  processor.setLfoFreq(freq);
}

void setLfoDepth(float depth)
{
  processor.setLfoDepth(depth);
}

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

bool ProcessAudio(int numInputs, const AudioSampleFrame *inputs,
                  int numOutputs, AudioSampleFrame *outputs,
                  int numParams, const AudioParamFrame *params,
                  void *userData)
{
  assert(numInputs == 0);
  assert(numOutputs == 1);
  assert(outputs->numberOfChannels == 1);
  assert(outputs->samplesPerChannel == WEB_AUDIO_FRAME_SIZE);
  auto &output = outputs[0];

  // // interleave input buffer (mono -> stereo)
  // for (int i = 0; i < WEB_AUDIO_FRAME_SIZE; ++i)
  // {
  //   interleavedIn[2 * i] = input.data[i];
  //   interleavedIn[2 * i + 1] = (inputs->numberOfChannels == 1) ? input.data[i] : input.data[i + WEB_AUDIO_FRAME_SIZE];
  // }

  for (int i = 0; i < numParams; ++i)
  {
    // Web simulator only: the per-parameter sliders drive the DSP through
    // the direct edit-parameter channel (Osc::simEditRequest, see wasm.cc
    // setEditParam), NOT through WebAudioParam slots 0..3. The slots are
    // kept in the descriptor only so the audio-worklet engine can
    // instantiate the node with the same shape the hardware uses (4
    // type_strings parameters), but their values are ignored here. On
    // hardware these slots are the PARAM/VALUE multiplexer and are driven
    // by the platform's unit_set_param_value() callback; the web simulator
    // has no platform, so the UI talks to the DSP directly.
    (void)i;
  }

  // emscripten_log(EM_LOG_CONSOLE, "bpm=%d", fx_get_bpmf());
  processor.process(nullptr, interleavedOut.data(), WEB_AUDIO_FRAME_SIZE);

  // de-interleave output buffer
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
                                                                                               "logue-osc", &options, &ProcessAudio, 0);

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
    // slider (drone.html) uses param.min/param.max directly and writes values
    // in that range. ProcessAudio rounds them to int32_t for setParameter().
    params[i].defaultValue = static_cast<float>(valid_parameters[i].init);
    params[i].minValue = static_cast<float>(valid_parameters[i].min);
    params[i].maxValue = static_cast<float>(valid_parameters[i].max);
  }

  WebAudioWorkletProcessorCreateOptions opts = {
      .name = "logue-osc",
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

// ---- Web-sim helpers: read/write selector cache state from the main thread ----
// IMPORTANT: The processor instance on the main thread and the processor instance
// on the audio worklet thread are SEPARATE COPIES (Emscripten creates isolated
// instances for the worklet despite SHARED_MEMORY=1). Therefore:
//   - getEditParamValue/getSelectParam read from the MAIN-THREAD processor
//   - setEditParam must ALSO write to the MAIN-THREAD processor (not via
//     simEditRequest, which is only consumed by the worklet-thread processor)
// A benign data race on 32-bit aligned ints is inaudible and only affects
// UI timing. The worklet-thread processor continues to run with its own
// (stale) state, but since the UI only reads via getEditParamValue (main
// thread), the display is correct.
extern "C" int getSelectParam()
{
  return static_cast<int>(processor.getSelect());
}

extern "C" int getEditParamValue(int idx)
{
  // While a Rnd re-roll morph is in flight (Osc::rndMorphTick), return the
  // CURRENT interpolated raw value (getEditMorphValue) so the sim's sliders
  // and display visibly "travel" toward the re-roll target instead of
  // jumping to it on the first poll. getEditMorphValue falls back to the
  // plain edit_value_ when no morph is active (the common case).
  return static_cast<int>(processor.getEditMorphValue(static_cast<uint8_t>(idx)));
}

// 1 while a Rnd re-roll morph is in flight (any parameter still moving or
// still in its stagger delay), 0 otherwise. The web simulator's post-fire
// slider-sync poll (websim/index.html fireRandom) uses this to know when to
// STOP: the poll must NOT stop on a "no value changed this frame" tick during
// the stagger window (before any parameter has started moving), because the
// morph is still in progress -- it must wait until rndMorphTick disarms
// morph_active_ (all parameters have reached their targets). Without this
// check the poll stopped after the very first tick (all 44 parameters were
// in their stagger delay, so edit_value_ had not changed yet), and the
// sliders never moved even though the worklet's morph was still running.
extern "C" int getMorphActive()
{
  return processor.isMorphActive() ? 1 : 0;
}

// Write one edit-parameter (index 0..kEditParamCount-1, raw 0..1023) DIRECTLY
// to the main-thread processor's multiplexer state. This updates both the
// edit_value_ cache AND the DSP parameters (via apply_edit_param) on the main
// thread, so getEditParamValue immediately reflects the new value.
//
// NOTE: With Emscripten's AUDIO_WORKLET + WASM_WORKERS + SHARED_MEMORY, the
// main thread and the worklet thread share the same linear memory (and hence
// the same global `processor` object). Writing to `processor` from the main
// thread is visible to the worklet on its next cache-reload (Params p = params_
// at the top of process()). A benign data race on 32-bit aligned ints is
// inaudible; the worklet may observe the new select_ one block later than the
// new edit_value_, which is at most one 2.9 ms block of stale state.
extern "C" void setEditParam(int idx, int raw)
{
  // Mimic the hardware's unit_set_param_value two-step:
  //   1. Select the parameter (PARAM_SEL = idx) -> updates select_ + re-applies
  //   2. Write the value (PARAM_VAL = raw)     -> updates edit_value_ + applies
  processor.setParameter(0, static_cast<int32_t>(idx)); // PARAM_SEL
  processor.setParameter(1, raw);                       // PARAM_VAL
}

// Fire the random re-roll on the audio-worklet thread (mirrors the hardware's
// "Rnd" trigger slot, but NOT debounced -- a single click is a single event).
// The web simulator's main-thread and audio-worklet processors are SEPARATE
// COPIES (see the comment in setEditParam), so a plain rndRequest() on the
// main-thread processor would only set its OWN rnd_fire_ flag, which the
// worklet never reads -- the re-roll would never fire. Instead, write the
// request to the main-thread processor's sim_rnd_request_ (a 32-bit packed
// slot: bit 31 = valid, bits 0..2 = mode); the worklet's process() consumes
// it on the next block (see osc_mix.h) by arming the worklet's own debounce
// with the requested mode. The mode is read from the main-thread processor
// (the UI keeps it in sync via setRndMode), so the trigger honours the
// CURRENT RndM type (All / Part / Orbi) without a separate mode argument.
// Same random windows (osc_rand() LCG) as the device.
extern "C" void rndTrigger()
{
  processor.simRndRequest(static_cast<uint8_t>(processor.getRndMode()));
}

// ---- Rnd randomization-type accessors (web-sim "Random" block) --------------
// The web simulator's Random block has a TYPE selector (All / Part / ORBI)
// and a TRIGGER. The type is a persistent setting (main-thread `processor`),
// the trigger fires on the audio thread (rndTrigger). Reading the type back
// from the main-thread processor keeps the sim UI in sync after a re-roll.
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
// The web sim's All/Partial/Reset preset buttons apply raw values through the
// main-thread setEditParam path (NOT the audio re-roll that stabilizes on its
// own), so after such an apply the C++ stable state would be stale -- the sim
// calls this once after the apply finishes, keeping ORBI anchored to whatever
// the sim is actually showing.
extern "C" void stableSync()
{
  processor.stableSync();
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

}
