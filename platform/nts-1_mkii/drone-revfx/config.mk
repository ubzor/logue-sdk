##############################################################################
# Configuration for Makefile
#

PROJECT := drone_revfx
PROJECT_TYPE := revfx

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

ULIBS  = -lm

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
