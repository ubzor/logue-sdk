##############################################################################
# Configuration for Makefile
#
# Web-simulator-only "drone-chain" module: the full DRONE audio path
# OSC -> MOD -> DEL -> REV -> out, run as a SINGLE wasm audio worklet.
#
# This project builds ONLY the wasm web simulator (see the `wasm` target in
# the Makefile). It is NOT an ARM unit: on the NTS-1 the four modules are
# separate units and the platform routes the audio itself. There is therefore
# no ARM toolchain / linker / install target here.

PROJECT := drone_chain

##############################################################################
# WASM (web simulator) sources
#
# C sources (compiled with emcc, plain C): header.c (the unit_header the
# worklet reads via getValidParameters) plus the wavetable / oscillator LUTs
# the OSC stage needs. On ARM these LUTs live in the NTS-1 firmware (the unit
# only references them); in the browser they must be linked in from the
# websim stubs, exactly as drone-osc does.
#
# C++ sources (compiled with em++, C++17 - required by the wasm audio worklet
# engine and libembind): wasm.cc (the embind binding + worklet bootstrap) and
# chain.cc (the Chain DSP). chain.h #includes the four production DSP headers
# (Osc / Modfx / Delay / Reverb) from the sibling drone-* module directories;
# those are added to the include path by the Makefile (UINCDIR).
#
WASMSRC_C := $(PROJECT_ROOT)/header.c \
             $(PROJECT_ROOT)/../../../websim/dsp/wavetable_lut.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-a.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-b.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-c.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-d.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-e.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-f.c
WASMSRC_CXX := $(PROJECT_ROOT)/wasm.cc $(PROJECT_ROOT)/chain.cc

##############################################################################
# Include Paths
#
# The four production DSP classes live in the sibling drone-* module dirs.
# chain.h includes them by bare name (#include "osc.h" / "modfx.h" / ...), so
# each of those directories must be on the include path. drone-osc's own dir
# is needed too (osc.h lives there).
#
UINCDIR  = $(PROJECT_ROOT) \
           $(PROJECT_ROOT)/../drone-osc \
           $(PROJECT_ROOT)/../drone-modfx \
           $(PROJECT_ROOT)/../drone-delfx \
           $(PROJECT_ROOT)/../drone-revfx

##############################################################################
# ARM placeholders (unused - this project is web-only). Keep the variables so
# the shared Makefile's ARM targets still have something defined; `make wasm`
# is the only target used.
#
UCSRC =
UCXXSRC =
UASMSRC =
UASMXSRC =
ULIBDIR =
ULIBS  =
UDEFS =
UCCOPT :=
UCXXOPT :=
