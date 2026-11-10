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

# FX modules ship no local unit.ld / version.map, so link against the shared
# platform linker script (same as the dummy-* templates). RULESPATH points the
# linker at the directory containing rules.ld (unit.ld INCLUDEs it).
LDSCRIPT := $(LDDIR)/unit.ld
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

# ARM C++ objects depend on the DSP headers: unit.cc (the only .cc in CXXSRC)
# includes them, so any change must force a recompile. (The WASM build already
# tracks them separately — see the WASMOBJDIR rule below.)
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
ALLOWED_UND := osc_white osc_rand wavesA wavesB wavesC wavesD wavesE wavesF \
               wt_sine_lut_f midi_to_hz_lut_f
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
		--shell-file $(SANDBOXDIR)/fx.html \
		-O2 \
		-g -fdebug-compilation-dir='..' \
		$(WASMOBJS) \
		-o $(WASMDIR)/$(PROJECT).html
	# The shell (fx.html) loads assets with relative paths (scripts/, images/),
	# and serve.sh serves $(WASMDIR) as the document root. Copy them so the
	# simulator is self-contained and does not depend on the websim/ tree.
	@echo Copying web assets to $(WASMDIR)
	@mkdir -p $(WASMDIR)/scripts $(WASMDIR)/images $(WASMDIR)/samples
	@cp -p $(SANDBOXDIR)/scripts/*.js $(WASMDIR)/scripts/
	@cp -p $(SANDBOXDIR)/images/*.ico $(WASMDIR)/images/ 2>/dev/null || true
	# fx.html fetches samples/samples.json (the demo audio sources) at load time,
	# so the samples dir must be present next to the HTML.
	@cp -p $(SANDBOXDIR)/samples/*.json $(WASMDIR)/samples/ 2>/dev/null || true
	@cp -p $(SANDBOXDIR)/samples/*.wav $(WASMDIR)/samples/ 2>/dev/null || true
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
	# drone.js fails at startup and the generated parameter UI never appears.
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
	@$(EMCC_BIN_PATH)/emcc -c $(INCDIR) -O2 -g -fdebug-compilation-dir='..' $< -o $@

# C++ sources are compiled with C++17 as required by the wasm audio worklet engine
$(WASMOBJDIR)/%.o: %.cc Makefile config.mk | $(WASMOBJDIR)
	@echo Compiling wasm C++ object $(<F)
	@$(EMCC_BIN_PATH)/em++ -c $(INCDIR) -fno-rtti -fno-exceptions -std=c++17 -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 -matomics -mbulk-memory -O2 -g -fdebug-compilation-dir='..' $< -o $@

$(WASMOBJDIR)/%.o: %.cpp | $(WASMOBJDIR)
	@echo Compiling wasm C++ object $(<F)
	@$(EMCC_BIN_PATH)/em++ -c $(INCDIR) -fno-rtti -fno-exceptions -std=c++17 -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 -matomics -mbulk-memory -O2 -g -fdebug-compilation-dir='..' $< -o $@