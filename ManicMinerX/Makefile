# ManicMinerX: Manic Miner + Manic Miner 128 in one XBE (nxdk + SDL2).
#
#   export NXDK_DIR=/path/to/nxdk
#   make -C ManicMinerX      # -> ManicMinerX/bin/default.xbe and ManicMinerX/ManicMinerX.iso
#
# At start-up it looks next to default.xbe for ManicMiner.bin and
# ManicMiner128.tap: one present -> that game starts; both -> a chooser
# styled on Manic Miner's title screen. Put the data files (and optionally
# 48.rom) in the repository's top folder before running make and they are
# copied next to default.xbe.

XBE_TITLE = ManicMinerX
GEN_XISO  = ManicMinerX.iso
NXDK_SDL  = y
TOP       = $(abspath $(CURDIR)/..)

SRCS = $(CURDIR)/main_x.c \
       $(CURDIR)/mm_game48.c \
       $(CURDIR)/mm_game128.c \
       $(TOP)/src/zx.c \
       $(TOP)/src/mm_font.c \
       $(TOP)/src/ay.c \
       $(TOP)/src/st_player.c \
       $(TOP)/src/mm128_load.c

CFLAGS += -O2 -Wall -I$(TOP)/src

NXDK_DIR ?= $(TOP)/../nxdk

include $(NXDK_DIR)/Makefile

GAME_DATA := $(wildcard $(TOP)/ManicMiner.bin $(TOP)/ManicMiner128.tap $(TOP)/48.rom)

.PHONY: gamedata
gamedata: $(OUTPUT_DIR)/default.xbe
	@for f in $(GAME_DATA); do cp "$$f" $(OUTPUT_DIR)/; echo "[ COPY     ] $$(basename $$f)"; done

$(GEN_XISO): gamedata
all: gamedata
