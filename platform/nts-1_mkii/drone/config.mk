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
# Note: no -lm. The unit no longer calls any libm function:
#   - powf() was replaced by a 4th-order Taylor series in update_w0_b()
#   - tanhf() was replaced by soft_tanhf() (a Padé [5/5] rational approx.)
# Both run entirely on the Cortex-M7 FPU with no external calls, which
# removes ~900 B of libm code (tanhf + expm1f + fabsf + __errno + helpers)
# and the associated dynamic symbols from the final .nts1mkiiunit.
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
#

UCCOPT := -ffunction-sections -fdata-sections -fno-ident
UCXXOPT := -ffunction-sections -fdata-sections -fno-ident

