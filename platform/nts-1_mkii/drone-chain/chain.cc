/*
 *  File: chain.cc
 *
 *  Translation unit that compiles the four production DSP headers (Osc /
 *  Modfx / Delay / Reverb, all #included by chain.h) as C++17 objects.
 *
 *  Why a separate TU from wasm.cc? The wasm audio-worklet engine (and the
 *  embind functions it wraps) must compile with the plain flags; but the DSP
 *  audio path runs on the dedicated audio thread and is best compiled with
 *  -matomics -mbulk-memory (the same flags drone-osc applies to its wasm C++
 *  objects). Keeping the DSP in its own object lets the Makefile give chain.cc
 *  those flags while wasm.cc stays the worklet bootstrap. In practice both are
 *  built with the same C++17 flags here, so this is mostly for clarity and to
 *  mirror the drone-osc build layout (unit.cc separate from wasm.cc).
 *
 *  Including chain.h here instantiates the Chain class template machinery and
 *  the inline DSP methods; the linker deduplicates the identical inline
 *  symbols against wasm.cc's own include. No symbols are defined twice at
 *  external linkage (the DSP is inline / static within the Chain object).
 */

#include "chain.h"
