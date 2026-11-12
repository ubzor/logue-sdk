##############################################################################
# Common project definitions
#

MKFILE_PATH := $(realpath $(lastword $(MAKEFILE_LIST)))

# Project root
PROJECT_ROOT ?= $(dir $(MKFILE_PATH))

# Common includes
COMMON_INC_PATH ?= $(realpath $(PROJECT_ROOT)/../common/)

# Common sources
COMMON_SRC_PATH ?= $(realpath $(PROJECT_ROOT)/../common/)

# Installation directory
INSTALLDIR ?= $(PROJECT_ROOT)

# Tools directory
TOOLSDIR ?= $(PROJECT_ROOT)/../../../tools

# External library directory
EXTDIR ?= $(PROJECT_ROOT)/../../ext

# CMSIS library location
CMSISDIR ?= $(EXTDIR)/CMSIS/CMSIS

# Linker scripts location
LDDIR ?= $(PROJECT_ROOT)/../ld

##############################################################################
# Include custom project configuration and sources
#

include config.mk

##############################################################################
# Common defaults
#

# Define project name here
PROJECT ?= my_unit

##############################################################################
# Setup cross compilation
#

MCU := cortex-m7

MCU_MODEL := STM32H725xE

GCC_TARGET := arm-none-eabi-
GCC_BIN_PATH ?= $(TOOLSDIR)/gcc/gcc-arm-none-eabi-10.3-2021.10/bin

CC    := $(GCC_BIN_PATH)/$(GCC_TARGET)gcc
CXXC  := $(GCC_BIN_PATH)/$(GCC_TARGET)g++
LD    := $(GCC_BIN_PATH)/$(GCC_TARGET)gcc
#LD   := $(GCC_BIN_PATH)/$(GCC_TARGET)g++
CP    := $(GCC_BIN_PATH)/$(GCC_TARGET)objcopy
AS    := $(GCC_BIN_PATH)/$(GCC_TARGET)gcc -x assembler-with-cpp
AR    := $(GCC_BIN_PATH)/$(GCC_TARGET)ar
OD    := $(GCC_BIN_PATH)/$(GCC_TARGET)objdump
SZ    := $(GCC_BIN_PATH)/$(GCC_TARGET)size
STRIP := $(GCC_BIN_PATH)/$(GCC_TARGET)strip

HEX   := $(CP) -O ihex
BIN   := $(CP) -O binary

# LDSCRIPT is set in config.mk to the local ./unit.ld (a copy of ../ld/unit.ld
# whose .unit_header section is not wrapped in KEEP(), so --gc-sections can
# drop the weak fallback unit_header from _unit_base.c). Do NOT override it
# here with $(LDDIR)/unit.ld, or the local script will be ignored.
RULESPATH := $(LDDIR)
DLIBS := -lc

DADEFS := -D$(MCU_MODEL) -DCORTEX_USE_FPU=TRUE -DARM_MATH_CM7
DDEFS := -D$(MCU_MODEL) -DCORTEX_USE_FPU=TRUE -DARM_MATH_CM7 -D__FPU_PRESENT

COPT := -fPIC -std=c11 -fno-exceptions
CXXOPT := -fPIC -fno-use-cxa-atexit -std=c++11 -fno-rtti -fno-exceptions -fno-non-call-exceptions

LDOPT := -shared --entry=0 -specs=nano.specs -specs=nosys.specs

CWARN := -W -Wall -Wextra
CXXWARN :=

FPU_OPTS := -mfloat-abi=hard -mfpu=fpv4-sp-d16 -fsingle-precision-constant -fcheck-new

OPT := -g -Os -mlittle-endian 
OPT += $(FPU_OPTS)

## TODO: there seems to be a bug or some yet unknown behavior that breaks PLT code for external calls when LTO is enabled alongside -nostartfiles
#OPT += -flto

TOPT := -mthumb -mno-thumb-interwork -DTHUMB_NO_INTERWORKING -DTHUMB_PRESENT

##############################################################################
# Set compilation targets and directories
#

PRODUCT := $(PROJECT).nts1mkiiunit

BUILDDIR := $(PROJECT_ROOT)/build
OBJDIR := $(BUILDDIR)/obj
LSTDIR := $(BUILDDIR)/lst

ASMSRC := $(UASMSRC)

ASMXSRC := $(UASMXSRC)

CSRC := $(UCSRC)
CSRC += $(realpath $(COMMON_SRC_PATH)/_unit_base.c)

CXXSRC := $(UCXXSRC)

vpath %.s $(sort $(dir $(ASMSRC)))
vpath %.S $(sort $(dir $(ASMXSRC)))
vpath %.c $(sort $(dir $(CSRC)))
vpath %.cc $(sort $(dir $(CXXSRC)))

ASMOBJS := $(addprefix $(OBJDIR)/, $(notdir $(ASMSRC:.s=.o)))
ASMXOBJS := $(addprefix $(OBJDIR)/, $(notdir $(ASMXSRC:.S=.o)))
COBJS := $(addprefix $(OBJDIR)/, $(notdir $(CSRC:.c=.o)))
CXXOBJS := $(addprefix $(OBJDIR)/, $(notdir $(CXXSRC:.cc=.o)))

OBJS := $(ASMXOBJS) $(ASMOBJS) $(COBJS) $(CXXOBJS)

DINCDIR := $(COMMON_INC_PATH) \
           $(CMSISDIR)/Include

INCDIR := $(patsubst %,-I%,$(DINCDIR) $(UINCDIR))

DEFS := $(DDEFS) $(UDEFS)
ADEFS := $(DADEFS) $(UADEFS)

LIBS := $(DLIBS) $(ULIBS)

LIBDIR := $(patsubst %,-I%,$(DLIBDIR) $(ULIBDIR))

##############################################################################
# Compiler flags
#

MCFLAGS   := -mcpu=$(MCU)
ODFLAGS	  := -x --syms
ASFLAGS   = $(MCFLAGS) -g $(TOPT) -Wa,-alms=$(LSTDIR)/$(notdir $(<:.s=.lst)) $(ADEFS)
ASXFLAGS  = $(MCFLAGS) -g $(TOPT) -Wa,-alms=$(LSTDIR)/$(notdir $(<:.S=.lst)) $(ADEFS)
CFLAGS    = $(MCFLAGS) $(TOPT) $(OPT) $(UCCOPT) $(COPT) $(CWARN) -Wa,-alms=$(LSTDIR)/$(notdir $(<:.c=.lst)) $(DEFS)
CXXFLAGS  = $(MCFLAGS) $(TOPT) $(OPT) $(UCXXOPT) $(CXXOPT) $(CXXWARN) -Wa,-alms=$(LSTDIR)/$(notdir $(<:.cc=.lst)) $(DEFS)
LDFLAGS   := $(MCFLAGS) $(TOPT) $(OPT) -nostartfiles $(LIBDIR) -Wl,-z,max-page-size=128,-Map=$(BUILDDIR)/$(PROJECT).map,--cref,--no-warn-mismatch,--library-path=$(RULESPATH),--script=$(LDSCRIPT) $(LDFLAGS_EXTRA) $(LDOPT)

OUTFILES := $(BUILDDIR)/$(PROJECT).elf \
	    $(BUILDDIR)/$(PROJECT).hex \
	    $(BUILDDIR)/$(PROJECT).bin \
	    $(BUILDDIR)/$(PROJECT).dmp \
	    $(BUILDDIR)/$(PROJECT).list

##############################################################################
# Targets
#

all: PRE_ALL $(OBJS) $(OUTFILES) POST_ALL
	@echo Done
	@echo

PRE_ALL:

POST_ALL:

$(OBJS): | $(BUILDDIR) $(OBJDIR) $(LSTDIR)

$(BUILDDIR):
	@echo Compiler Options
	@echo $(CC) -c $(CFLAGS) -I. $(INCDIR)
	@echo
	@mkdir -p $(BUILDDIR)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(LSTDIR):
	@mkdir -p $(LSTDIR)

$(ASMOBJS) : $(OBJDIR)/%.o : %.s Makefile
	@echo Assembling $(<F)
	@$(AS) -c $(ASFLAGS) -I. $(INCDIR) $< -o $@

$(ASMXOBJS) : $(OBJDIR)/%.o : %.S Makefile
	@echo Assembling $(<F)
	@$(CC) -c $(ASXFLAGS) -I. $(INCDIR) $< -o $@

$(COBJS) : $(OBJDIR)/%.o : %.c Makefile config.mk
	@echo Compiling $(<F)
	@$(CC) -c $(CFLAGS) -I. $(INCDIR) $< -o $@

# ARM C++ objects depend on the DSP headers: unit.cc (the only .cc in CXXSRC)
# includes them, so any change must force a recompile. (The WASM build already
# tracks them separately — see the WASMOBJDIR rule below.)
$(CXXOBJS) : $(OBJDIR)/%.o : %.cc Makefile config.mk modfx.h
	@echo Compiling $(<F)
	@$(CXXC) -c $(CXXFLAGS) -I. $(INCDIR) $< -o $@

$(BUILDDIR)/%.elf: $(OBJS) $(LDSCRIPT)
	@echo Linking $@
	@echo $(LD) $(OBJS) $(LDFLAGS) $(LIBS) -o $@
	@$(LD) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

%.hex: %.elf
	@echo Creating $@
	@$(HEX) $< $@

%.bin: %.elf
	@echo Creating $@
	@$(BIN) $< $@

%.dmp: %.elf
	@echo Creating $@
	@$(OD) $(ODFLAGS) $< > $@
	@echo
	@$(SZ) $<
	@echo

%.list: %.elf
	@echo Creating $@
	@$(OD) -S $< > $@

clean:
	@echo Cleaning
	-rm -fR $(PROJECT_ROOT)/.dep $(BUILDDIR) $(PROJECT_ROOT)/$(PRODUCT)
	@echo Done
	@echo

$(BUILDDIR)/$(PRODUCT): | $(OBJS) $(OUTFILES)
	@echo Making $(BUILDDIR)/$(PRODUCT)
	@cp -a $(BUILDDIR)/$(PROJECT).elf $(BUILDDIR)/$(PRODUCT)
	@$(STRIP) $(BUILDDIR)/$(PRODUCT)

# ---- Undefined-symbol (UND) guard ------------------------------------------
# The NTS-1 mkII runtime resolves a unit's undefined dynamic symbols by name at
# load time and only knows the unit_* API + the firmware LUTs. Any OTHER
# undefined symbol (e.g. a stray libm powf/floorf/expf) makes the device reject
# the unit with "ELF: unresolved symbol". This guard lists the UND symbols in
# the built artifact and fails the build if any appear outside the allowed set,
# so a regression is caught at build time instead of on the device.
#
# ALLOWED_UND: the symbols the firmware is expected to provide. Add a new one
# here ONLY when the firmware genuinely provides it.
# FX modules use no firmware LUTs, so this list is normally empty: any
# undefined dynamic symbol (e.g. a stray libm powf/sinf) means the unit will
# fail to load on device. The exception is the platform white-noise API
# (fx_rand / fx_white, declared in common/fx_api.h and linked into every fx
# unit by the runtime) -- the NOISE branch and the DRIFT walk consume them.
ALLOWED_UND := fx_rand fx_white
READELF := $(GCC_BIN_PATH)/$(GCC_TARGET)readelf

check-und: $(BUILDDIR)/$(PRODUCT)
	@echo Checking undefined dynamic symbols in $(BUILDDIR)/$(PRODUCT)
	@UND=$$($(READELF) --dyn-syms $(BUILDDIR)/$(PRODUCT) 2>/dev/null | awk '/ UND / {print $$8}' | grep -v '^$$' | sort -u); \
	BAD=""; \
	for s in $$UND; do \
	  case " $(ALLOWED_UND) " in *" $$s "*) ;; *) BAD="$$BAD $$s";; esac; \
	done; \
	if [ -n "$$BAD" ]; then \
	  echo "ERROR: unexpected undefined symbols (the device will fail to load this unit):" >&2; \
	  for b in $$BAD; do echo "  - $$b" >&2; done; \
	  echo "Replace the call with an inlined fast equivalent (see config.mk) or" >&2; \
	  echo "add the symbol to ALLOWED_UND in the Makefile if the firmware provides it." >&2; \
	  exit 1; \
	fi; \
	echo "OK: only firmware-provided symbols are undefined ($$(echo $$UND | tr '\n' ' '))"

install: check-und $(BUILDDIR)/$(PRODUCT)
	@echo Deploying to $(INSTALLDIR)/$(PRODUCT)
	@mv $(BUILDDIR)/$(PRODUCT) $(INSTALLDIR)/$(PRODUCT)
	@echo Done
	@echo

# The web simulator for the drone family is the unified chain page
# (drone-chain/, one wasm worklet running OSC -> MOD -> DEL -> REV). This
# module no longer has a standalone simulator: there is no wasm target here.