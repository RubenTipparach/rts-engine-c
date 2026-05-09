# rts-engine-c

C/Sokol port of [rts-engine][upstream] (C# / Silk.NET / Blazor WASM).
A space RTS with a dynamic solar system, terrain-based planet view, star
map, and unit sim. Targets native desktop and the web (WebGL2).

[upstream]: https://github.com/RubenTipparach/rts-engine

The C# original is vendored at `reference/rts-engine/` as a read-only
git submodule and used as the spec for the port. See `CLAUDE.md` for
the repo rules.

## Build

```bash
git clone --recurse-submodules https://github.com/RubenTipparach/rts-engine-c.git
cd rts-engine-c

# Native (Linux/macOS):
make             # → build/native/rts-engine
make run

# Web (requires emscripten in PATH):
make web         # → build-web/index.html
make serve       # http://localhost:8000
```

### Linux deps

```
sudo apt install libgl-dev libxi-dev libxcursor-dev
```

### macOS

Xcode command line tools (`xcode-select --install`) are sufficient.

## Layout

```
src/                C engine code
  main.c            sokol_app entry, frame loop
  sokol_impl.c      single TU that defines SOKOL_IMPL
  core/             logging, math, config (YAML loader)
  render/           solar system / planet / star map renderers
  sim/              orbital + RTS sim
  gen/              generated shader headers (sokol-shdc output)
assets/             runtime content (configs, shaders, planet YAMLs, …)
third_party/        vendored single-header libs (sokol_*.h, HandmadeMath.h)
tools/              build helpers + content generators
reference/rts-engine/   read-only C# reference (submodule)
```

## Status

Foundation. Window opens, clears to deep-space colour on both targets.
Solar-system rendering, YAML-driven planet config, camera/zoom, and the
RTS sim land in subsequent commits — see CI / open PRs.

## Links

- Play (web build): https://rubentipparach.github.io/rts-engine-c/
- CI builds: https://github.com/RubenTipparach/rts-engine-c/actions
