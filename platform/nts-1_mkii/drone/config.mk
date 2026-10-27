##############################################################################
# Configuration for Makefile
#

PROJECT := drone
PROJECT_TYPE := osc

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

ULIBS  = -lm

##############################################################################
# Macros
#

UDEFS = 

