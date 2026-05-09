# rts-engine-c — Makefile
#
# Targets:
#   make            same as `make native`
#   make native     desktop build (Linux/macOS auto-detected)
#   make web        emscripten WebGL2 build (writes build-web/index.html)
#   make run        run the native binary
#   make serve      serve the web build at http://localhost:8000
#   make clean      remove build outputs
#
# Notes:
#   - The native build picks the platform default backend: GL on Linux,
#     Metal on macOS. (D3D11 on Windows is not wired yet.)
#   - The web build is fixed to WebGL2 via emscripten. Files under
#     `assets/` are preloaded into the virtual FS as `/assets/`.
#   - sokol_impl.c is the *only* TU that pulls in sokol's impl blocks.
#     On macOS it's compiled as Objective-C; the rest of the project
#     compiles as plain C on every platform.

UNAME_S := $(shell uname -s)

CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
INCS    := -Ithird_party -Ithird_party/sokol -Isrc

# Plain C sources (compiled the same on every platform).
SRC_C := \
	src/main.c \
	src/core/log.c \
	src/render/solarsystem.c

# Sokol implementation TU — platform-specific flags applied separately.
SRC_SOKOL := src/sokol_impl.c

OBJDIR_NATIVE := build/native
BIN_NATIVE    := $(OBJDIR_NATIVE)/rts-engine
WEB_OUT       := build-web/index.html

OBJ_C     := $(SRC_C:src/%.c=$(OBJDIR_NATIVE)/%.o)
OBJ_SOKOL := $(OBJDIR_NATIVE)/sokol_impl.o
OBJ_ALL   := $(OBJ_C) $(OBJ_SOKOL)

ifeq ($(UNAME_S),Linux)
	BACKEND       := SOKOL_GLCORE
	LDLIBS        := -lX11 -lXi -lXcursor -lGL -ldl -lpthread -lm
	# clock_gettime / CLOCK_MONOTONIC live behind a POSIX feature gate
	# under -std=c11; expose them for sokol_time.h and sokol_app.h.
	CFLAGS        += -D_POSIX_C_SOURCE=199309L
	SOKOL_CFLAGS  :=
endif
ifeq ($(UNAME_S),Darwin)
	BACKEND       := SOKOL_METAL
	LDLIBS        := -framework Cocoa -framework QuartzCore -framework Metal \
	                 -framework MetalKit -framework AudioToolbox
	# sokol_impl.c on macOS must compile as Objective-C so sokol_app.h
	# can use Cocoa/Metal — but only this one TU.
	SOKOL_CFLAGS  := -x objective-c -fobjc-arc
endif

NATIVE_DEFS := -D$(BACKEND)

EMCC      ?= emcc
EMCFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
WEB_DEFS  := -DSOKOL_GLES3
WEB_LINK  := -sUSE_WEBGL2=1 -sFULL_ES3=1 -sALLOW_MEMORY_GROWTH=1 \
             --preload-file assets@/assets --shell-file tools/shell.html

.PHONY: all native web run serve clean

all: native

native: $(BIN_NATIVE)

$(BIN_NATIVE): $(OBJ_ALL) | $(OBJDIR_NATIVE)
	$(CC) $(OBJ_ALL) $(LDLIBS) -o $@

$(OBJDIR_NATIVE)/%.o: src/%.c | $(OBJDIR_NATIVE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) $(NATIVE_DEFS) -c $< -o $@

$(OBJ_SOKOL): $(SRC_SOKOL) | $(OBJDIR_NATIVE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SOKOL_CFLAGS) $(INCS) $(NATIVE_DEFS) -c $< -o $@

$(OBJDIR_NATIVE):
	mkdir -p $(OBJDIR_NATIVE)

run: native
	./$(BIN_NATIVE)

web: $(WEB_OUT)

# Web build is single-shot through emcc — emscripten handles GL portability,
# so no Objective-C and no per-TU split is needed.
$(WEB_OUT): $(SRC_C) $(SRC_SOKOL) tools/shell.html
	mkdir -p build-web
	$(EMCC) $(EMCFLAGS) $(INCS) $(WEB_DEFS) $(SRC_C) $(SRC_SOKOL) $(WEB_LINK) -o $(WEB_OUT)

serve: web
	cd build-web && python3 -m http.server 8000

clean:
	rm -rf build build-web
