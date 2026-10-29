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
vpath %.c $(sort $(dir $(CSRC)) $(dir $(WASMSRC_C)))
vpath %.cc $(sort $(dir $(CXXSRC)) $(dir $(WASMSRC_CXX)))
vpath %.cpp $(sort $(dir $(WASMSRC_CXX)))

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
# WebAssembly compiler and binding tools
#
SANDBOXDIR ?= $(PROJECT_ROOT)/../../../websim
# WASM sources: C and C++ are compiled separately because the wasm audio
# worklet engine (and libembind) require C++17, while plain C sources must
# not be compiled with a C++ standard in this emscripten version.
WASMSRC_C ?= $(UCSRC)
WASMSRC_CXX ?= $(UCXXSRC)
WASMDIR := $(PROJECT_ROOT)/sim
DINCDIR += $(TOOLSDIR)/emsdk/upstream/emscripten/system/include
EMCC_BIN_PATH := $(TOOLSDIR)/emsdk/upstream/emscripten
WASMOBJDIR := $(WASMDIR)/obj

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

$(CXXOBJS) : $(OBJDIR)/%.o : %.cc Makefile config.mk
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
	-rm -fR $(PROJECT_ROOT)/.dep $(BUILDDIR) $(PROJECT_ROOT)/$(PRODUCT) $(WASMDIR)
	@echo Done
	@echo

$(BUILDDIR)/$(PRODUCT): | $(OBJS) $(OUTFILES)
	@echo Making $(BUILDDIR)/$(PRODUCT)
	@cp -a $(BUILDDIR)/$(PROJECT).elf $(BUILDDIR)/$(PRODUCT)
	@$(STRIP) $(BUILDDIR)/$(PRODUCT)

install: $(BUILDDIR)/$(PRODUCT)
	@echo Deploying to $(INSTALLDIR)/$(PRODUCT)
	@mv $(BUILDDIR)/$(PRODUCT) $(INSTALLDIR)/$(PRODUCT)
	@echo Done
	@echo

WASMOBJS := $(addprefix $(WASMOBJDIR)/, $(notdir $(WASMSRC_C:.c=.o)) $(notdir $(WASMSRC_CXX:.cc=.o)))

wasm: $(WASMOBJS)
	@echo Building WebAssembly audio processor
	@echo Linking $(WASMDIR)/$(PROJECT).html
	@$(EMCC_BIN_PATH)/em++ -Wno-unknown-attributes -Wno-limited-postlink-optimizations $(INCDIR) \
		-fno-rtti -fno-exceptions -std=c++17 -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 \
		-s AUDIO_WORKLET=1 \
		-s WASM_WORKERS=1 \
		-s SHARED_MEMORY=1 \
		-lembind \
		--shell-file $(SANDBOXDIR)/osc.html \
		-O2 \
		-g -fdebug-compilation-dir='..' \
		$(WASMOBJS) \
		-o $(WASMDIR)/$(PROJECT).html
	# The shell (osc.html) loads assets with relative paths (scripts/, images/),
	# and serve.sh serves $(WASMDIR) as the document root. Copy them so the
	# simulator is self-contained and does not depend on the websim/ tree.
	@echo Copying web assets to $(WASMDIR)
	@mkdir -p $(WASMDIR)/scripts $(WASMDIR)/images
	@cp -p $(SANDBOXDIR)/scripts/*.js $(WASMDIR)/scripts/
	@cp -p $(SANDBOXDIR)/images/*.ico $(WASMDIR)/images/ 2>/dev/null || true
	@echo Done

$(WASMOBJDIR):
	@mkdir -p $(WASMOBJDIR)

# C sources are compiled as plain C (the audio worklet engine below is C++17)
$(WASMOBJDIR)/%.o: %.c | $(WASMOBJDIR)
	@echo Compiling wasm C object $(<F)
	@$(EMCC_BIN_PATH)/emcc -c $(INCDIR) -O2 -g -fdebug-compilation-dir='..' $< -o $@

# C++ sources are compiled with C++17 as required by the wasm audio worklet engine
$(WASMOBJDIR)/%.o: %.cc Makefile config.mk osc.h | $(WASMOBJDIR)
	@echo Compiling wasm C++ object $(<F)
	@$(EMCC_BIN_PATH)/em++ -c $(INCDIR) -fno-rtti -fno-exceptions -std=c++17 -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 -matomics -mbulk-memory -O2 -g -fdebug-compilation-dir='..' $< -o $@

$(WASMOBJDIR)/%.o: %.cpp | $(WASMOBJDIR)
	@echo Compiling wasm C++ object $(<F)
	@$(EMCC_BIN_PATH)/em++ -c $(INCDIR) -fno-rtti -fno-exceptions -std=c++17 -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 -matomics -mbulk-memory -O2 -g -fdebug-compilation-dir='..' $< -o $@