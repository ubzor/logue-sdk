##############################################################################
# drone-chain Makefile (WEB-ONLY)
#
# Builds the "drone-chain" wasm web simulator: the full DRONE audio path
# OSC -> MOD -> DEL -> REV -> out running as a single wasm audio worklet
# (see chain.h / wasm.cc). This project is NOT an ARM unit -- there is no
# cross-compiler / linker / install target here, only `make wasm`.
#
# Usage:
#   make wasm    # build sim/index.js + index.wasm, sync into app/static/
#   make clean
#
##############################################################################

MKFILE_PATH := $(realpath $(lastword $(MAKEFILE_LIST)))

# Project root
PROJECT_ROOT ?= $(dir $(MKFILE_PATH))

# Common includes (processor.h, unit_osc.h, runtime.h, etc.)
COMMON_INC_PATH ?= $(realpath $(PROJECT_ROOT)/../common/)

# Tools directory (emsdk lives here)
TOOLSDIR ?= $(PROJECT_ROOT)/../../../tools

##############################################################################
# Include custom project configuration (PROJECT, WASMSRC, UINCDIR, ...)
#
include config.mk

# This is a web-only project: the "product" is the simulator, not an ARM unit.
PRODUCT := $(PROJECT)

##############################################################################
# WebAssembly compiler and binding tools
#
SANDBOXDIR ?= $(PROJECT_ROOT)/../../../websim
# WASM sources: C and C++ are compiled separately because the wasm audio
# worklet engine (and libembind) require C++17, while plain C sources must
# not be compiled with a C++ standard in this emscripten version.
WASMSRC_C ?=
WASMSRC_CXX ?=
WASMDIR := $(PROJECT_ROOT)/sim
DINCDIR := $(COMMON_INC_PATH) $(UINCDIR)
INCDIR := $(patsubst %,-I%,$(DINCDIR))
EMCC_BIN_PATH := $(TOOLSDIR)/emsdk/upstream/emscripten
WASMOBJDIR := $(WASMDIR)/obj

# C++ flags for the wasm audio-worklet engine / embind / DSP. -std=c++17 is
# required by the worklet engine and libembind; -matomics -mbulk-memory for the
# audio thread (same as drone-osc's wasm objects); -fno-rtti -fno-exceptions.
#
# -DEMSCRIPTEN: the DSP headers (osc.h) gate their web-simulator-only members
# (the shape-LFO synthesizer setLfoFreq/setLfoDepth, the sim edit/rnd request
# slots, etc.) behind #ifdef EMSCRIPTEN. emscripten defines EMSCRIPTEN for the
# files it compiles directly (wasm.cc), but chain.cc is precompiled to an
# object by our pattern rule, so we must set it explicitly -- otherwise Chain
# would not see those members. This is a web-only build, so defining it is
# correct for every wasm object.
CXXFLAGS_WASM := -DEMSCRIPTEN -fno-rtti -fno-exceptions -std=c++17 -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 -matomics -mbulk-memory -O2 -g -fdebug-compilation-dir='..'
CFLAGS_WASM   := -O2 -g -fdebug-compilation-dir='..'

##############################################################################
# Object lists
#
# Each source maps to one object: <basename>.o in $(WASMOBJDIR). The pattern
# rules ($(WASMOBJDIR)/%.o: %.cc etc.) use the stem WITHOUT the source
# extension (e.g. stem "wasm" for "wasm.o" target + "wasm.cc" prerequisite),
# so the .o names must strip the .c/.cc/.cpp extension. The vpath lines below
# tell make where to find each source.
# Each .c/.cc/.cpp source maps to exactly one .o name via patsubst, so the
# C++ list can hold any of the three extensions without double-matching: a
# substitution reference like $(X:.cc=.o) would ALSO rewrite the ".cpp"
# suffix of .cpp files and add a bogus "wasm.cc" target, while patsubst
# only rewrites the listed suffix and leaves other extensions untouched.
WASMOBJS := $(addprefix $(WASMOBJDIR)/, \
    $(patsubst %.c,%.o,$(notdir $(WASMSRC_C))) \
    $(patsubst %.cc,%.o,$(filter %.cc,$(notdir $(WASMSRC_CXX)))) \
    $(patsubst %.cpp,%.o,$(filter %.cpp,$(notdir $(WASMSRC_CXX)))))

# Source lookup paths for the pattern rules below: the wasm objects are written
# into $(WASMOBJDIR) (flat), but the sources live in $(PROJECT_ROOT) and in
# websim/dsp/ (the LUT C files). vpath tells make where to find %.c / %.cc.
vpath %.c $(sort $(dir $(WASMSRC_C)) $(dir $(WASMSRC_CXX)))
vpath %.cc $(sort $(dir $(WASMSRC_CXX)))
vpath %.cpp $(sort $(dir $(WASMSRC_CXX)))

##############################################################################
# Targets
#
# The emscripten glue is emitted as sim/index.js; the SvelteKit app in
# websim/app/ serves it (plus index.wasm) from the document root, so the
# landing URL is http://host:8000/.
# NOTE: keep recipe comments parenthesis-free -- /bin/sh (dash) on some
# build machines rejects unquoted '(' inside recipe comment lines.
#
.PHONY: wasm clean

wasm: $(WASMOBJS)
	@echo 'Building WebAssembly audio processor (drone-chain)'
	@echo Linking $(WASMDIR)/index.js
	@$(EMCC_BIN_PATH)/em++ -Wno-unknown-attributes -Wno-limited-postlink-optimizations $(INCDIR) \
		-fno-rtti -fno-exceptions -std=c++17 -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 \
		-s AUDIO_WORKLET=1 \
		-s WASM_WORKERS=1 \
		-s SHARED_MEMORY=1 \
		-lembind \
		-O2 \
		-g -fdebug-compilation-dir='..' \
		$(WASMOBJS) \
		-o $(WASMDIR)/index.js
	# The emscripten glue (index.js) is the artifact the SvelteKit app in
	# $(SANDBOXDIR)/app serves from the document root (plus index.wasm). The
	# legacy HTML shell (websim/index.html) is gone: the UI is the SvelteKit
	# app (app/src/), and the glue's boot call is rewired to the app's audio
	# wiring module by app/scripts/relink-glue.mjs (run by serve.sh).
	@echo 'Syncing emscripten artifacts into the SvelteKit app (app/static/)'
	@mkdir -p $(SANDBOXDIR)/app/static
	@cp -p $(WASMDIR)/index.js $(SANDBOXDIR)/app/static/index.js
	@cp -p $(WASMDIR)/index.wasm $(SANDBOXDIR)/app/static/index.wasm
	@echo Done

$(WASMOBJDIR):
	@mkdir -p $(WASMOBJDIR)

# C sources are compiled as plain C (the audio worklet engine below is C++17)
$(WASMOBJDIR)/%.o: %.c | $(WASMOBJDIR)
	@echo Compiling wasm C object $(<F)
	@$(EMCC_BIN_PATH)/emcc -c $(INCDIR) $(CFLAGS_WASM) $< -o $@

# C++ sources are compiled with C++17 as required by the wasm audio worklet
# engine. chain.cc / wasm.cc both include the DSP headers (Osc / Modfx /
# Delay / Reverb) from the sibling drone-* dirs (see UINCDIR in config.mk).
#
# CRITICAL: those DSP headers are in SIBLING module dirs and are NOT under
# drone-chain, so a naive "%.o: %.cc" rule would never recompile the object
# when, say, drone-modfx/modfx.h changes -- make only sees chain.cc (unchanged)
# and links the STALE object, so DSP edits silently fail to reach the sim.
# The DSP_HEADERS var below lists every header the C++ TU actually includes,
# and is added as a prerequisite so any DSP edit invalidates chain.o/wasm.o.
DSP_HEADERS := $(PROJECT_ROOT)/chain.h \
               $(PROJECT_ROOT)/wasm.cc \
               $(PROJECT_ROOT)/hwfilter.h \
               $(PROJECT_ROOT)/../drone-osc/osc.h \
               $(PROJECT_ROOT)/../drone-osc/osc_mix.h \
               $(PROJECT_ROOT)/../drone-osc/osc_lifecycle.h \
               $(PROJECT_ROOT)/../drone-osc/osc_lfo_sim.h \
               $(PROJECT_ROOT)/../drone-osc/osc_params.h \
               $(PROJECT_ROOT)/../drone-osc/osc_param_apply.h \
               $(PROJECT_ROOT)/../drone-osc/osc_param_display.h \
               $(PROJECT_ROOT)/../drone-osc/osc_constants.h \
               $(PROJECT_ROOT)/../drone-osc/osc_waves.h \
               $(PROJECT_ROOT)/../drone-osc/osc_saturator.h \
               $(PROJECT_ROOT)/../drone-osc/osc_svf.h \
               $(PROJECT_ROOT)/../drone-osc/osc_envelope.h \
               $(PROJECT_ROOT)/../drone-osc/osc_detune.h \
               $(PROJECT_ROOT)/../drone-osc/osc_sub2.h \
               $(PROJECT_ROOT)/../drone-osc/osc_random_walk.h \
               $(PROJECT_ROOT)/../drone-osc/osc_randomize.h \
               $(PROJECT_ROOT)/../drone-osc/osc_rnd.h \
               $(PROJECT_ROOT)/../drone-modfx/modfx.h \
               $(PROJECT_ROOT)/../drone-modfx/modfx_constants.h \
               $(PROJECT_ROOT)/../drone-delfx/delay.h \
               $(PROJECT_ROOT)/../drone-revfx/reverb.h

$(WASMOBJDIR)/%.o: %.cc Makefile config.mk $(DSP_HEADERS) | $(WASMOBJDIR)
	@echo Compiling wasm C++ object $(<F)
	@$(EMCC_BIN_PATH)/em++ -c $(INCDIR) $(CXXFLAGS_WASM) $< -o $@

$(WASMOBJDIR)/%.o: %.cpp Makefile config.mk $(DSP_HEADERS) | $(WASMOBJDIR)
	@echo Compiling wasm C++ object $(<F)
	@$(EMCC_BIN_PATH)/em++ -c $(INCDIR) $(CXXFLAGS_WASM) $< -o $@

clean:
	@echo Cleaning
	-rm -fR $(WASMDIR)
	@echo Done
	@echo
