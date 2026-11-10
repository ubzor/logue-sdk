##############################################################################
# drone-chain Makefile (WEB-ONLY)
#
# Builds the "drone-chain" wasm web simulator: the full DRONE audio path
# OSC -> MOD -> DEL -> REV -> out running as a single wasm audio worklet
# (see chain.h / wasm.cc). This project is NOT an ARM unit -- there is no
# cross-compiler / linker / install target here, only `make wasm`.
#
# Usage:
#   make wasm    # build sim/index.html + index.js + index.wasm (+ assets)
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
# The simulator page is emitted as index.html so serve.sh can serve the sim/
# directory as the document root and land on http://host:8000/ [index.html].
# NOTE: keep recipe comments parenthesis-free -- /bin/sh (dash) on some
# build machines rejects unquoted '(' inside recipe comment lines.
#
.PHONY: wasm clean

wasm: $(WASMOBJS)
	@echo 'Building WebAssembly audio processor (drone-chain)'
	@echo Linking $(WASMDIR)/index.html
	@$(EMCC_BIN_PATH)/em++ -Wno-unknown-attributes -Wno-limited-postlink-optimizations $(INCDIR) \
		-fno-rtti -fno-exceptions -std=c++17 -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 \
		-s AUDIO_WORKLET=1 \
		-s WASM_WORKERS=1 \
		-s SHARED_MEMORY=1 \
		-lembind \
		--shell-file $(SANDBOXDIR)/index.html \
		-O2 \
		-g -fdebug-compilation-dir='..' \
		$(WASMOBJS) \
		-o $(WASMDIR)/index.html
	# The shell index.html loads assets with relative paths [scripts/, images/,
	# samples/], and serve.sh serves $(WASMDIR) as the document root. Copy them
	# so the simulator is self-contained and does not depend on the websim/ tree.
	@echo Copying web assets to $(WASMDIR)
	@mkdir -p $(WASMDIR)/scripts $(WASMDIR)/images $(WASMDIR)/samples
	@cp -p $(SANDBOXDIR)/scripts/*.js $(WASMDIR)/scripts/
	@cp -p $(SANDBOXDIR)/images/*.ico $(WASMDIR)/images/ 2>/dev/null || true
	@cp -p $(SANDBOXDIR)/samples/*.json $(WASMDIR)/samples/ 2>/dev/null || true
	@cp -p $(SANDBOXDIR)/samples/*.wav $(WASMDIR)/samples/ 2>/dev/null || true
	@cp -p $(SANDBOXDIR)/samples/*.ogg $(WASMDIR)/samples/ 2>/dev/null || true
	@cp -p $(SANDBOXDIR)/samples/*.mp3 $(WASMDIR)/samples/ 2>/dev/null || true
	@echo 'Building FlyonUI assets (Vite) in $(SANDBOXDIR)'
	@if [ ! -d "$(SANDBOXDIR)/node_modules" ]; then \
		echo "node_modules missing - running npm install in $(SANDBOXDIR)"; \
		cd "$(SANDBOXDIR)" && npm install --no-audit --no-fund; \
	fi
	@cd "$(SANDBOXDIR)" && npx vite build
	@cp -p "$(SANDBOXDIR)/sim/flyon.css" "$(WASMDIR)/flyon.css"
	@cp -p "$(SANDBOXDIR)/sim/flyon.js" "$(WASMDIR)/flyon.js"
	# Vercel deploy config: the wasm build uses -s SHARED_MEMORY=1 (see above),
	# which needs SharedArrayBuffer, i.e. the page must be cross-origin isolated.
	# Vercel does not send these headers by default (unlike serve.sh, which adds
	# them in the Python handler), so without vercel.json the simulator's
	# index.js fails at startup and the generated parameter UI never appears.
	# sim/ is git-ignored and wiped on rebuild, so this file must be generated
	# after the build, not kept in the repo.
	@echo 'Writing Vercel config (COOP/COEP headers) to $(WASMDIR)/vercel.json'
	@printf '%s\n' \
	'{' \
	'  "headers": [' \
	'    {' \
	'      "source": "/(.*)",' \
	'      "headers": [' \
	'        { "key": "Cross-Origin-Opener-Policy", "value": "same-origin" },' \
	'        { "key": "Cross-Origin-Embedder-Policy", "value": "require-corp" },' \
	'        { "key": "Cross-Origin-Resource-Policy", "value": "same-origin" }' \
	'      ]' \
	'    }' \
	'  ]' \
	'}' > "$(WASMDIR)/vercel.json"
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
$(WASMOBJDIR)/%.o: %.cc Makefile config.mk | $(WASMOBJDIR)
	@echo Compiling wasm C++ object $(<F)
	@$(EMCC_BIN_PATH)/em++ -c $(INCDIR) $(CXXFLAGS_WASM) $< -o $@

$(WASMOBJDIR)/%.o: %.cpp Makefile config.mk | $(WASMOBJDIR)
	@echo Compiling wasm C++ object $(<F)
	@$(EMCC_BIN_PATH)/em++ -c $(INCDIR) $(CXXFLAGS_WASM) $< -o $@

clean:
	@echo Cleaning
	-rm -fR $(WASMDIR)
	@echo Done
	@echo
