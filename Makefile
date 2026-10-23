# ROTH.C Chinese localisation mod
# ---------------------------------------------------------------------------
# Builds 32-bit plugin.dll against the ROTH.C plugin SDK.
#
#   make                  build plugin.dll
#   make clean            remove plugin.dll
#   make install          copy plugin.dll + font + translation table into DIST_WIN
#   make install GAME=1   ... and then into the GOG install's mods/chinese
#
# Toolchain: any 32-bit mingw-w64 gcc, e.g. w64devkit (put its bin/ on PATH).
#
# The SDK include path is the ABI source of truth (struct roth_plugin_info_v1,
# struct roth_chain, the generated override typedefs in roth_sdk_overrides.h).
# Point ROTH_SDK at the SDK root of your ROTH.C checkout:
#
#   make ROTH_SDK=/path/to/ROTH.C/sdk
#
# so the default only works for this working tree.

ROTH_SDK ?= ../../repos/ROTH.C/sdk
SDK_INC  := $(ROTH_SDK)/include

CROSS    ?= mingw
ifeq ($(CROSS),mingw)
  CC     := i686-w64-mingw32-gcc
else
  CC     ?= cc
endif

DIST_WIN ?= dist/mods/chinese
# Optional extra target for the local game install. Unset by default; point it
# at <game-dir>/ROTH/mods/chinese and build with `make install GAME=1`.
GOG_MODS ?=

# FONT_SRC is the one TrueType face the plugin actually loads (see FONT_FILE in
# plugin.c). The other faces in fonts/ are kept only for experiments and are NOT
# installed -- shipping all of them would add ~76 MB for no runtime benefit.
FONT_SRC ?= fonts/WenQuanYi Bitmap Song 14px.ttf

# -D_GNU_SOURCE        : match the SDK example build (see sdk/examples).
# -fno-stack-protector : the override is reachable from the 70 Hz SIGALRM tick
#                        (sdk/MODDING_GUIDE.md "tick_isr_reachable"); a stack
#                        canary here would pull in __stack_chk_fail and is not
#                        wanted in signal context.
# -fno-asynchronous-unwind-tables : we never throw/unwind; dropping the .eh_frame
#                        keeps the DLL small and avoids a libgcc dependency.
# NOTE: no -ffreestanding -- this mod deliberately uses libc (fopen/fprintf for
# the debug log, memcpy, snprintf), so the freestanding flag would be a lie.
CFLAGS  := -shared -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -D_GNU_SOURCE -fno-stack-protector -fno-asynchronous-unwind-tables \
           -I$(SDK_INC)

plugin.dll: plugin.c stb_truetype.h
	$(CC) $(CFLAGS) -o $@ plugin.c

clean:
	-rm -f plugin.dll

install: plugin.dll
	mkdir -p "$(DIST_WIN)"
	cp -f plugin.dll "$(DIST_WIN)/plugin.dll"
	cp -f "$(FONT_SRC)" "$(DIST_WIN)/"
	cp -f translations_zh.bin "$(DIST_WIN)/"
	@echo "installed -> $(DIST_WIN)"
ifeq ($(GAME),1)
	mkdir -p "$(GOG_MODS)"
	cp -f plugin.dll translations_zh.bin "$(GOG_MODS)/"
	cp -f "$(FONT_SRC)" "$(GOG_MODS)/"
	@echo "installed -> $(GOG_MODS)"
endif

.PHONY: clean install
