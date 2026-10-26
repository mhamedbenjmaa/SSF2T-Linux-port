# Super Street Fighter II Turbo - Linux port.
#
#   make GAME=/path/to/game     first build: generates src/recomp/ from the
#                               game's SF2TURBO.EXE (needs Python 3 + capstone)
#   make                        later builds
#
# GAME can also come from the SSF2T_GAME environment variable.
CC      ?= gcc
PYTHON  ?= python3
GAME    ?= $(SSF2T_GAME)
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 -Wall -Iinclude -Isrc/recomp -fno-strict-aliasing -fwrapv
LDLIBS  += -ldl -lm -lpthread
GENFLAGS = -O1 -std=gnu11 -Iinclude -Isrc/recomp -fno-strict-aliasing -fwrapv -w

STAMP := src/recomp/.generated

all: $(STAMP)
	@$(MAKE) --no-print-directory sf2t

# The recompiled game code is generated from the user's own copy of the game,
# never stored in the repository.
$(STAMP): tools/recomp.py tools/function-ranges.json overrides.txt
	@if [ -z "$(GAME)" ]; then \
	  echo "Generating the game code needs the original game:"; \
	  echo "  make GAME=/path/to/game   (the folder with SF2TURBO.EXE)"; exit 1; fi
	@test -f "$(GAME)/SF2TURBO.EXE" || { echo "no SF2TURBO.EXE in $(GAME)"; exit 1; }
	@echo "Generating src/recomp from $(GAME)/SF2TURBO.EXE (about 30 s)..."
	$(PYTHON) tools/recomp.py "$(GAME)/SF2TURBO.EXE" tools/function-ranges.json src/recomp
	@touch $@

GEN  := $(wildcard src/recomp/gen_*.c)
SRC  := src/recomp/runtime.c src/recomp/links.c src/recomp/functable.c \
        $(wildcard src/platform/*.c) $(wildcard src/game/*.c)
OBJ  := $(GEN:src/%.c=build/%.o) $(SRC:src/%.c=build/%.o)

sf2t: $(OBJ)
	$(CC) -o $@ $^ $(LDLIBS)

build/recomp/gen_%.o: src/recomp/gen_%.c src/recomp/recomp.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c -o $@ $<

build/%.o: src/%.c src/recomp/recomp.h src/platform/platform.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build sf2t

# also removes the generated game code
distclean: clean
	rm -f src/recomp/gen_*.c src/recomp/funcs.h src/recomp/functable.c \
	      src/recomp/links.c src/recomp/apis.h $(STAMP)

.PHONY: all clean distclean
