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
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
INCS    := -Ithird_party -Ithird_party/sokol -Isrc

# Plain C sources (compiled the same on every platform).
SRC_C := \
	src/main.c \
	src/core/config.c \
	src/core/log.c \
	src/core/noise.c \
	src/render/camera.c \
	src/render/goldberg.c \
	src/render/solarsystem.c \
	src/render/sphere.c

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
	# under -std=gnu11; expose them for sokol_time.h and sokol_app.h.
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
EMCFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
WEB_DEFS  := -DSOKOL_GLES3
WEB_LINK  := -sUSE_WEBGL2=1 -sFULL_ES3=1 -sALLOW_MEMORY_GROWTH=1 \
             --preload-file assets@/assets --shell-file tools/shell.html

# sokol-shdc is fetched on demand by `make shaders`. The generated
# headers under src/gen/*.glsl.h are committed so a clean clone builds
# without needing the binary locally — only re-run `make shaders` after
# editing assets/shaders/*.glsl.
SHDC_VERSION ?= master
SHDC_DIR     := .tools
SHDC         := $(SHDC_DIR)/sokol-shdc
SHDC_OS      := linux
ifeq ($(UNAME_S),Darwin)
ifeq ($(shell uname -m),arm64)
	SHDC_OS := osx_arm64
else
	SHDC_OS := osx
endif
endif
SHDC_TARGETS := glsl410:glsl300es

GEN_DIR    := src/gen
GEN_HDRS   := $(GEN_DIR)/sun.glsl.h $(GEN_DIR)/solarsystem.glsl.h \
              $(GEN_DIR)/orbit.glsl.h $(GEN_DIR)/atmosphere.glsl.h \
              $(GEN_DIR)/starfield.glsl.h

.PHONY: all native web run serve clean shaders

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

# Regenerate the sokol-shdc headers from assets/shaders/*.glsl. The
# binary is downloaded on first use into .tools/ (gitignored). The
# default `make` does NOT depend on this; run it after editing a
# shader and commit the resulting headers under src/gen/.
shaders: $(GEN_HDRS)

$(SHDC):
	@mkdir -p $(SHDC_DIR)
	@echo "fetching sokol-shdc ($(SHDC_OS), $(SHDC_VERSION))…"
	@curl -sL --fail "https://github.com/floooh/sokol-tools-bin/archive/refs/heads/$(SHDC_VERSION).tar.gz" \
		| tar -xz -C $(SHDC_DIR) --strip-components=3 \
		"sokol-tools-bin-$(SHDC_VERSION)/bin/$(SHDC_OS)/sokol-shdc"
	@chmod +x $(SHDC)

$(GEN_DIR)/%.glsl.h: assets/shaders/%.glsl | $(SHDC)
	@mkdir -p $(GEN_DIR)
	$(SHDC) -i $< -o $@ -l $(SHDC_TARGETS)
