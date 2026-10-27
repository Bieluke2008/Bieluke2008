#---------------------------------------------------------------------------------
# Nintendo Switch build (devkitPro / devkitA64 + libnx). Produces openfront.nro.
#
#   ./scripts/fetch-deps.sh        # OpenFront + QuickJS-ng sources
#   ./scripts/build-romfs.sh       # engine.js, maps, languages, fonts -> romfs/
#   make                           # needs DEVKITPRO (or use scripts/build-switch.sh)
#
# For a desktop build, see CMakeLists.txt.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET		:=	openfront
BUILD		:=	build/switch
SOURCES		:=	src deps/quickjs deps/cjson
INCLUDES	:=	src deps/quickjs deps/cjson
ROMFS		:=	romfs
ICON		:=	icon.jpg

APP_TITLE	:=	OpenFront
APP_AUTHOR	:=	OpenFront contributors (unofficial Switch port)
# The one place the version lives: VERSION (also read by CMakeLists.txt).
APP_VERSION	:=	$(shell cat $(TOPDIR)/VERSION)

# QuickJS-ng ships tools and tests next to the library; only these are built.
QJS_CFILES	:=	quickjs.c libregexp.c libunicode.c dtoa.c

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	`$(PREFIX)pkg-config --cflags sdl2 SDL2_ttf SDL2_image SDL2_mixer` -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES) -D_GNU_SOURCE

CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -DOPENFRONT_VERSION=\"$(APP_VERSION)\"

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:=	`$(PREFIX)pkg-config --libs SDL2_mixer SDL2_image SDL2_ttf sdl2` -lz -lnx -lm

LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# Top level when run from the project, build level when run from $(BUILD).
ifneq ($(notdir $(BUILD)),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(notdir $(wildcard src/*.c)) $(QJS_CFILES) cJSON.c

export LD	:=	$(CXX)

export OFILES	:=	$(CFILES:.c=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/$(ICON)
export NROFLAGS += --icon=$(APP_ICON)
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -f $(ROMFS)/engine.js ] || { echo "romfs/ is not ready: run scripts/build-romfs.sh first"; exit 1; }
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf	:	$(OFILES)

# QuickJS is third-party code: keep its warnings out of our build log.
quickjs.o libregexp.o libunicode.o dtoa.o: CFLAGS += -w
# DetMath's exp/log must round like the TS: no fused multiply-add.
native.o: CFLAGS += -ffp-contract=off

-include $(DEPENDS)

endif
