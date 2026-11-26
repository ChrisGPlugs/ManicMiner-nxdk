# Manic Miner for the original Xbox (nxdk + SDL2).
#
#   export NXDK_DIR=/path/to/nxdk
#   make                      # -> bin/default.xbe and ManicMiner.iso
#
# Put ManicMiner.bin (and optionally 48.rom) in this folder before running
# make and they are copied next to default.xbe and into the ISO.

XBE_TITLE = Manic\ Miner
XBE_TITLEID = 4D4D0058    # shared with ManicMinerX: one save folder (cxbe writes FFFF0002; patched after)
GEN_XISO  = ManicMiner.iso
NXDK_SDL  = y

SRCS = $(CURDIR)/src/main.c \
       $(CURDIR)/src/zx.c \
       $(CURDIR)/src/mm_game.c \
       $(CURDIR)/src/mm_font.c \
       $(CURDIR)/src/hiscore.c

CFLAGS += -O2 -Wall

NXDK_DIR ?= $(CURDIR)/../nxdk

include $(NXDK_DIR)/Makefile

# Copy the game data next to the XBE (before the ISO is built from bin/).
GAME_DATA := $(wildcard $(CURDIR)/ManicMiner.bin $(CURDIR)/48.rom)

.PHONY: titleid gamedata
titleid: $(OUTPUT_DIR)/default.xbe
	@sh $(CURDIR)/tools/xbe_titleid.sh $(OUTPUT_DIR)/default.xbe $(XBE_TITLEID)

gamedata: titleid
	@for f in $(GAME_DATA); do cp "$$f" $(OUTPUT_DIR)/; echo "[ COPY     ] $$(basename $$f)"; done

$(GEN_XISO): gamedata
all: gamedata
