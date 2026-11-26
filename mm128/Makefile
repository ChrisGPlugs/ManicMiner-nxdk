# Manic Miner 128 (Mihai Novitchi / Bob Fossil, Software Projects version)
# for the original Xbox (nxdk + SDL2).
#
#   export NXDK_DIR=/path/to/nxdk
#   make -C mm128            # -> mm128/bin/default.xbe and mm128/ManicMiner128.iso
#
# Put ManicMiner128.tap (and optionally 48.rom) in the repository's top
# folder before running make and they are copied next to default.xbe.

XBE_TITLE = Manic\ Miner\ 128
XBE_TITLEID = 4D4D0058    # shared with ManicMinerX: one save folder (cxbe writes FFFF0002; patched after)
GEN_XISO  = ManicMiner128.iso
NXDK_SDL  = y
TOP       = $(abspath $(CURDIR)/..)

SRCS = $(CURDIR)/main128.c \
       $(CURDIR)/mm_game128.c \
       $(TOP)/src/zx.c \
       $(TOP)/src/mm_font.c \
       $(TOP)/src/hiscore.c \
       $(TOP)/src/ay.c \
       $(TOP)/src/st_player.c \
       $(TOP)/src/mm128_load.c

CFLAGS += -O2 -Wall -I$(TOP)/src

NXDK_DIR ?= $(TOP)/../nxdk

include $(NXDK_DIR)/Makefile

GAME_DATA := $(wildcard $(TOP)/ManicMiner128.tap $(TOP)/48.rom)

.PHONY: titleid gamedata
titleid: $(OUTPUT_DIR)/default.xbe
	@sh $(TOP)/tools/xbe_titleid.sh $(OUTPUT_DIR)/default.xbe $(XBE_TITLEID)

gamedata: titleid
	@for f in $(GAME_DATA); do cp "$$f" $(OUTPUT_DIR)/; echo "[ COPY     ] $$(basename $$f)"; done

$(GEN_XISO): gamedata
all: gamedata
