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

// ---- Web-sim stub for the platform noise API --------------------------------
// On ARM, osc_white() is provided by the runtime (osc_api.h extern). In the
// web simulator there is no runtime, so we provide a lightweight white-noise
// source based on the Park-Miller-Carta LCG (matching websim/dsp/osc_api.cpp).
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
    // K-rate parameter: use the first sample for the frame.
    // The Web Audio Param carries the unit's native integer range [min,max]
    // (see AudioThreadInitialized). Round the first sample to the nearest
    // integer and forward it, exactly as the hardware runtime does
    // (setParameter takes a 10-bit integer, not a float).
    const float v = params[i].data[0];
    const AudioWorkletParameter p = valid_parameters_ref()[i];
    const int ival = static_cast<int>(v + 0.5f);
    const int clamped = (ival < p.min) ? p.min : (ival > p.max) ? p.max : ival;
    processor.setParameter(i, clamped);
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