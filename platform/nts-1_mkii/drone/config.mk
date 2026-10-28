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
WASMSRC_C := $(PROJECT_ROOT)/header.c
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

