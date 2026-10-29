##############################################################################
# Configuration for Makefile
#

PROJECT := drone
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

##############################################################################
# WASM (web simulator) sources
#
# C sources (compiled with emcc, plain C)
#
# C++ sources (compiled with em++, C++17 - required by the wasm audio
# worklet engine and libembind in this emscripten version)
#
# The web simulator has no NTS-1 runtime, so any LUT the runtime would
# normally provide must be linked in from the websim stubs:
#   * wavetable_lut.c  -> wt_sine_lut_f (osc_sinf, sub-octave sine)
#   * waves-a..f.c     -> wavesA..wavesF (the 91 wavetables scanned by the
#                         two wavetable oscillators, osc_wave_scanf)
# On ARM these live in the NTS-1 mkII firmware, so the unit only references
# them (a few pointer reads). In the browser they add ~46 KB to drone.wasm
# (91 tables x 129 samples x 4 bytes) — a small fraction of the ~1 MB total.
WASMSRC_C := $(PROJECT_ROOT)/header.c \
             $(PROJECT_ROOT)/../../../websim/dsp/wavetable_lut.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-a.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-b.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-c.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-d.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-e.c \
             $(PROJECT_ROOT)/../../../websim/dsp/waves-f.c
WASMSRC_CXX := $(PROJECT_ROOT)/wasm.cc

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
# Note: no -lm. The unit does not pull in any libm code:
#   - powf() was replaced by a 4th-order Taylor series in update_w0_b()
#   - tanhf() was replaced by soft_tanhf() (a Padé [5/5] rational approx.)
#   - sqrtf() (random-walk step, once per 128-sample block) is inlined by the
#     compiler into the single Cortex-M7 FMSQRT instruction via -fno-math-errno
#     (see UCXXOPT below), so no libm sqrtf / __errno / PLT entry is linked.
# All math runs entirely on the Cortex-M7 FPU, keeping the final .nts1mkiiunit
# free of libm code and dynamic symbols.
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

