##############################################################################
# Configuration for Makefile
#

PROJECT := drone_osc
PROJECT_TYPE := osc

##############################################################################
# Size optimizations (local to this unit)
#
# - LDSCRIPT: local copy of ../ld/unit.ld (INCLUDEs the local rules.ld) whose
#   .unit_header section is not wrapped in KEEP(), so --gc-sections can drop
#   the weak fallback unit_header defined in common/_unit_base.c (~200 bytes).
# - LDFLAGS_EXTRA:
#   * --version-script=version.map restricts the dynamic symbol table to
#     unit_header and unit_* (the only symbols the NTS-1 runtime resolves),
#     shrinking .dynsym/.dynstr/.hash and removing their PLT entries.
#   * --gc-sections drops unreferenced input sections.
#
# - Unit metadata (author / version) lives in the ELF header struct:
#   dev_id encodes the author name as displayed on device ('oobz'), unit_id
#   is the unit number within the author's scope (0x00000001 = first unit),
#   and version is the unit's major.minor.patch. These are set in header.c,
#   so bump the unit counter / version there when shipping new versions.
#

LDSCRIPT := $(PROJECT_ROOT)/unit.ld

LDFLAGS_EXTRA := \
  -Wl,--version-script=$(PROJECT_ROOT)/version.map \
  -Wl,--gc-sections

# WASM (web simulator) sources: this module no longer builds a standalone
# simulator -- the web simulator for the whole family is the unified chain
# page (drone-chain/, one wasm worklet running OSC -> MOD -> DEL -> REV,
# see drone-chain/config.mk). The LUT sources listed there
# (websim/dsp/wavetable_lut.c, waves-a..f.c) exist for that build: on ARM they
# live in the NTS-1 mkII firmware, so the unit only references them
# (wt_sine_lut_f, wavesA..F) via the firmware LUTs.

##############################################################################
# Sources
#

# C sources 
UCSRC = header.c

# C++ sources 
UCXXSRC = unit.cc

# List ASM source files here
UASMSRC = 

UASMXSRC = 

##############################################################################
# Include Paths
#

UINCDIR  = 

##############################################################################
# Library Paths
#

ULIBDIR = 

##############################################################################
# Libraries
#
# Note: no -lm. The unit must pull in NO libm code, because the NTS-1 mkII
# runtime resolves a unit's undefined dynamic symbols by name at load time and
# only knows the unit_* API + the firmware LUTs (osc_white, wavesA..F,
# wt_sine_lut_f, midi_to_hz_lut_f). Any libm symbol left undefined (e.g. powf,
# floorf, expf) becomes an "ELF: unresolved symbol" error and the unit fails to
# load. We therefore replace every scalar libm call with a fast, inlined
# equivalent (see the headers):
#   - powf(x, 2.0)  -> x*x                (FM quadratic curve, osc_mix/param_display)
#   - powf(a, b)    -> fasterpowf(a, b)   (FCUT log sweep, osc_param_apply)
#   - expf(x)       -> fasterexpf(x)      (envelope / sub2 coefficients)
#   - floorf(x)     -> fast_floorf(x)     (modular phasor wrap, osc.h)
#   - tanhf()       -> soft_tanhf()       (Padé [5/5] rational approx.)
#   - sqrtf()       -> inlined FMSQRT via -fno-math-errno (see UCXXOPT)
# All math runs on the Cortex-M7 FPU, keeping the .nts1mkiiunit free of libm
# dynamic symbols. The `install` target also runs a UND-symbol guard that fails
# the build if any unexpected unresolved symbol reappears (see Makefile).
#

ULIBS  =

##############################################################################
# Macros
#

UDEFS = 

##############################################################################
# Per-language compiler options (size)
#
# -ffunction-sections/-fdata-sections: put every function/data object in its
#   own section so --gc-sections can remove the unused ones at link time.
# -fno-ident: drop the .comment (compiler id) section from the output.
# -fno-math-errno (C++/unit.cc): let the compiler emit the Cortex-M7 FMSQRT
#   instruction directly for sqrtf() instead of calling libm sqrtf(). This is
#   safe (no errno side effects in an audio unit) and removes the libm sqrtf
#   code, its PLT entry, and the __errno dependency from the final image.
#   (The C sources need no math calls, so only UCXXOPT sets it.)
#

UCCOPT := -ffunction-sections -fdata-sections -fno-ident
UCXXOPT := -ffunction-sections -fdata-sections -fno-ident -fno-math-errno

