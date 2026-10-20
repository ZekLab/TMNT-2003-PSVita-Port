# Mac host build of the translated TMNT_EN.EXE: make -j8 (generated C must exist: recomp/gen_c.py + recomp/gen_tables.py).
GEN := recomp/out/c
OBJ := build/mac/obj
CC := clang
GENFLAGS := -O1 -w -std=gnu11 -Irt -DGUEST_GENERATED_STACK_GUARD=0 -DGUEST_GPR_LOCAL=1
RTFLAGS := -O1 -g -Wall -Wno-unused-function -std=gnu11 -Irt -DGUEST_GENERATED_STACK_GUARD=0 $(shell sdl2-config --cflags)

GEN_SRC := $(wildcard $(GEN)/guest_*.c) $(GEN)/dispatch.c $(GEN)/imports.c
RT_SRC := rt/runtime.c rt/win32.c rt/com.c rt/d3d9.c rt/d3d9_gl.c rt/plat_sdl.c rt/dinput.c rt/dsound.c rt/game_tmnt.c
GEN_OBJ := $(patsubst $(GEN)/%.c,$(OBJ)/gen/%.o,$(GEN_SRC))
RT_OBJ := $(patsubst rt/%.c,$(OBJ)/rt/%.o,$(RT_SRC))

build/mac/tmnt: $(GEN_OBJ) $(RT_OBJ)
	$(CC) -o $@ $^ -lm -lpthread -lz $(shell sdl2-config --libs) -framework OpenGL

$(OBJ)/gen/%.o: $(GEN)/%.c rt/guest.h rt/game_hooks.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c $< -o $@

$(OBJ)/rt/%.o: rt/%.c rt/guest.h rt/runtime.h rt/com.h rt/d3d9.h rt/plat.h
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

clean:
	rm -rf build/mac
