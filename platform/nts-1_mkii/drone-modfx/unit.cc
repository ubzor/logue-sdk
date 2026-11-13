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
 *  File: unit.cc
 *
 *  NTS-1 mkII modfx effect unit interface
 *
 */

#include "modfx.h"
#include "unit_modfx.h"     // base definitions for modfx units
#include "utils/int_math.h" // clipminmaxi32()
#include <algorithm>        // std::fill

static Modfx s_processor_instance; // actual instance of custom delay object

static int32_t cached_values[UNIT_MODFX_MAX_PARAM_COUNT]; // cached parameter values passed from hardware

// ---- Callbacks exposed to runtime ----------------------------------------------

__unit_callback int8_t unit_init(const unit_runtime_desc_t *desc)
{
  if (!desc)
    return k_unit_err_undef;

  // Note: make sure the unit is being loaded to the correct platform/module target
  if (desc->target != unit_header.target)
    return k_unit_err_target;

  // Note: check API compatibility with the one this unit was built against
  if (!UNIT_API_IS_COMPAT(desc->api))
    return k_unit_err_api_version;

  // Check compatibility of samplerate with unit
  if (desc->samplerate != s_processor_instance.getSampleRate())
    return k_unit_err_samplerate;

  // Check compatibility of frame geometry
  if (desc->input_channels != 2 || desc->output_channels != 2) // should be stereo input/output
    return k_unit_err_geometry;

  // If SDRAM buffers are required they must be allocated here
  if (!desc->hooks.sdram_alloc)
    return k_unit_err_memory;

  if (s_processor_instance.getBufferSize() > 0)
  {
    float *allocated_buffer_ = (float *)desc->hooks.sdram_alloc(s_processor_instance.getBufferSize() * sizeof(float));
    if (!allocated_buffer_)
      return k_unit_err_memory;

    // clear buffer
    std::fill(allocated_buffer_, allocated_buffer_ + s_processor_instance.getBufferSize(), 0.f);
    s_processor_instance.init(allocated_buffer_);
  }
  else
  {
    s_processor_instance.init(nullptr);
  }

  // initialize cached parameters to defaults
  for (int id = 0; id < UNIT_MODFX_MAX_PARAM_COUNT; ++id)
  {
    cached_values[id] = static_cast<int32_t>(unit_header.params[id].init);
  }

  return k_unit_err_none;
}

__unit_callback void unit_teardown()
{
  s_processor_instance.teardown();
}

__unit_callback void unit_reset()
{
  s_processor_instance.reset();
}

__unit_callback void unit_resume()
{
  s_processor_instance.resume();
}

__unit_callback void unit_suspend()
{
  s_processor_instance.suspend();
}

__unit_callback void unit_render(const float *in, float *out, uint32_t frames)
{
  s_processor_instance.process(in, out, frames);
}

__unit_callback void unit_set_param_value(uint8_t id, int32_t value)
{
  // clip to valid range as defined in header
  value = clipminmaxi32(unit_header.params[id].min, value, unit_header.params[id].max);

  // cache value for unit_get_param_value(id)
  cached_values[id] = value;

  // The 6 header slots map onto the Modfx's 6 parameter ids:
  //   slot 0 (TIME,  fixed)    -> PARAM_TIME  (0)
  //   slot 1 (DEPTH, fixed)    -> PARAM_DEPTH (1)
  //   slot 2 (PARAM, optional) -> PARAM_SEL   (2)  MOD multiplexer selector
  //   slot 3 (VALUE, optional) -> PARAM_VAL   (3)  raw value of select_
  //   slot 4 (TPARAM,optional) -> PARAM_TSEL  (4)  TAPE multiplexer selector
  //   slot 5 (TVALUE,optional) -> PARAM_TVAL  (5)  raw value of tsel_
  // Modfx::setParameter only ever sees PARAM_TIME / PARAM_DEPTH / PARAM_SEL /
  // PARAM_VAL / PARAM_TSEL / PARAM_TVAL (the 2 fixed + the two multiplexer
  // pairs; the 10 edit parameters live behind them). The slot index already
  // equals the parameter id (0..5), so forward 1:1.
  s_processor_instance.setParameter(id, value);
}

__unit_callback int32_t unit_get_param_value(uint8_t id)
{
  // VALUE slot (3) reports the cached raw value of the currently selected MOD
  // edit parameter (the multiplexer's live state, like drone-osc's getValue()).
  // TVALUE slot (5) reports the currently selected TAPE edit parameter. PARAM /
  // TPARAM slots (2/4) report the cached selector values. TIME / DEPTH (0/1)
  // report their own cached values.
  if (id == 3U)
    return s_processor_instance.getValue();
  if (id == 5U)
    return s_processor_instance.getTapeValue();
  return cached_values[id];
}

__unit_callback const char *unit_get_param_str_value(uint8_t id, int32_t value)
{
  value = clipminmaxi32(unit_header.params[id].min, value, unit_header.params[id].max); // just in case
  return s_processor_instance.getParameterStrValue(id, value);
}

__unit_callback void unit_set_tempo(uint32_t tempo)
{
  float bpm = (tempo >> 16) + (tempo & 0xFFFF) / static_cast<float>(0x10000);
  s_processor_instance.setTempo(bpm);
}

__unit_callback void unit_tempo_4ppqn_tick(uint32_t counter)
{
  s_processor_instance.tempo4ppqnTick(counter);
}