# ROTH.C Chinese localisation mod
# ---------------------------------------------------------------------------
# i686-w64-mingw32 (F:\mingw\w64devkit) -> 32-bit plugin.dll
#
#   make            build plugin.dll
#   make clean      remove plugin.dll
#   make install    copy plugin.dll + font + translation table into dist-win
#
# The include path points at the engine's SDK, which is the ABI source of truth
# (struct roth_plugin_info_v1, struct roth_chain, the generated override
# typedefs in roth_sdk_overrides.h).

SDK_INCLUDE := ../../repos/ROTH.C/sdk/include
CC          := i686-w64-mingw32-gcc
DIST_WIN    ?= ../../repos/ROTH.C/roth_c/dist-win/mods/chinese

CFLAGS := -shared -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
          -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables \
          -I$(SDK_INCLUDE)

plugin.dll: plugin.c stb_truetype.h
	$(CC) $(CFLAGS) -o $@ plugin.c

clean:
	-rm -f plugin.dll

install: plugin.dll
	mkdir -p "$(DIST_WIN)"
	cp -f plugin.dll "$(DIST_WIN)/plugin.dll"
	cp -f fonts/*.ttf "$(DIST_WIN)/"
	if [ -f OFL.txt ]; then cp -f OFL.txt "$(DIST_WIN)/"; fi
	if [ -f translations_zh.bin ]; then cp -f translations_zh.bin "$(DIST_WIN)/"; fi
	@echo "installed -> $(DIST_WIN)"

.PHONY: clean install
