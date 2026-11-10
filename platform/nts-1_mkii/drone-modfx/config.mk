##############################################################################
# Configuration for Makefile
#

PROJECT := drone_modfx
PROJECT_TYPE := modfx

##############################################################################
# Size optimizations (local to this unit)
#
# Same strategy as drone-osc:
# - LDSCRIPT: local ./unit.ld (INCLUDEs the local rules.ld) whose .unit_header
#   section is NOT wrapped in KEEP(), so --gc-sections can drop the weak
#   fallback unit_header from common/_unit_base.c.
# - LDFLAGS_EXTRA:
#   * --version-script=version.map restricts the dynamic symbol table to
#     unit_header and unit_* (the only symbols the NTS-1 runtime resolves),
#     shrinking .dynsym/.dynstr/.hash and removing their PLT entries.
#   * --gc-sections drops unreferenced input sections.
#
# - No -lm: the NTS-1 mkII runtime resolves a unit's undefined dynamic symbols
#   by name at load time and only knows the unit_* API + firmware LUTs. FX
#   units use no LUTs, so ULIBS is empty. The check-und target in the Makefile
#   fails the build if any unexpected UND symbol appears.
#
# - Per-language flags: -ffunction-sections/-fdata-sections put every
#   function/data object in its own section so --gc-sections can remove the
#   unused ones at link time. -fno-ident drops the .comment section.
#   -fno-math-errno lets the compiler emit FMSQRT for sqrtf() on Cortex-M7.
#
LDSCRIPT := $(PROJECT_ROOT)/unit.ld

LDFLAGS_EXTRA := \
  -Wl,--version-script=$(PROJECT_ROOT)/version.map \
  -Wl,--gc-sections

UCCOPT := -ffunction-sections -fdata-sections -fno-ident
UCXXOPT := -ffunction-sections -fdata-sections -fno-ident -fno-math-errno

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

ULIBS  =

##############################################################################
# Macros
#

UDEFS =

##############################################################################
# WASM (web simulator) sources
#
# FX modules have no runtime-provided LUTs (the dummy/osc DSP stubs are not
# needed). WASMSRC_C compiles header.c (the unit_header), WASMSRC_CXX compiles
# unit.cc + wasm.cc (the embind binding). The shell is websim/fx.html.
#
WASMSRC_C := $(PROJECT_ROOT)/header.c
WASMSRC_CXX := $(PROJECT_ROOT)/unit.cc $(PROJECT_ROOT)/wasm.cc
