# Silk.NET → Sokol parity tracker

Living checklist of what still needs to be ported from the C# / Silk.NET /
Blazor reference (`reference/rts-engine/`) to the active C / Sokol port at
the repo root. Item granularity is "one focused slice" — each box should
land in roughly one commit / branch.

Status legend: `[ ]` not started · `[~]` partial · `[x]` done.

## M1 — Solar system view (current milestone)

- [x] sokol_app + sokol_gfx + sokol_glue shell, GL/Metal/D3D11/WebGL2 backend select
- [x] YAML loader (engine.yaml, solarsystem.yaml, planets/*.yaml)
- [x] Orbit camera (azimuth/elevation/distance), click-pick + zoom transition
- [x] Touch parity (1-finger drag = orbit, pinch = zoom, tap = pick)
- [x] Sun shader (flat sphere, Bayer dither) — `assets/shaders/sun.glsl`
- [x] Planet + moon spheres with orbital motion — `src/render/solarsystem.c`
- [x] Orbit rings — `assets/shaders/orbit.glsl`
- [x] Starfield background — `assets/shaders/starfield.glsl`
- [x] Debug HUD (FPS, focus, touch diagnostics) via `sokol_debugtext`
- [ ] Sun corona pass + bloom-ish glow (`glowColor`/`glowRadius`/`coronaSpeed`
      from `solarsystem.yaml` are parsed but unused) — extend `sun.glsl`

## M2 — Scene-mode switching + planet edit view

Foundation work that unblocks everything below.

- [ ] `scene_mode_t { SOLAR, PLANET_EDIT, STAR_MAP, RTS_GROUND }` state
      machine + cross-fade transition (duration from
      `engine.yaml:camera.transitionDuration`) — new `src/core/scene_mode.{h,c}`
- [ ] Per-mode dispatch in `src/main.c` (frame, event, HUD label)
- [ ] On-screen mode-switch button (touch-reachable per CLAUDE.md) +
      keybind (e.g. `Tab`)
- [ ] Auto-switch SOLAR → PLANET_EDIT when zoom crosses the per-planet
      `zoomMin` threshold; reverse via
      `engine.yaml:planetEditView.autoZoomOutThreshold`

### Planet mesh + biomes

- [ ] Connect existing `src/render/goldberg.c` cell generator to a real
      draw path (currently UV sphere only) — bind cell mesh + per-cell
      level index
- [ ] Biome shader: 6-level terrain colour ramp from
      `planets/*.yaml:noiseThresholds` + `levels[]`
- [ ] Cliff geometry between levels (step height from
      `planet.yaml:stepHeight`) and chamfered top fan
      (`engine.yaml:terrain.chamferInset/chamferDrop`)
- [ ] Slope placement: deterministic, density from
      `engine.yaml:slopes.density`, seed from `slopes.seedOffset` + planet seed
- [ ] Cell outline overlay (fade by `engine.yaml:lod.outlineMaxDist`)

### Texturing pipeline

- [ ] `tools/gen_terrain_atlas.py` — bake per-biome 16×16 or 32×32 PNG tiles
      into a single atlas under `assets/textures/terrain/` (pixel-art rules
      from CLAUDE.md)
- [ ] Atlas binding + per-level UV lookup in the biome shader

### LOD

- [ ] Distance-based LOD switch: full planet mesh inside
      `lod.planetMaxDist`, solar-system sphere beyond
- [ ] Cross-fade band between `lod.transitionBlendStart` and
      `lod.transitionBlendEnd`

### Atmosphere

- [~] `assets/shaders/atmosphere.glsl` authored (Nishita single-scatter,
      8 view × 4 light samples)
- [ ] Wire atmosphere shell pass to PLANET_EDIT render path
- [ ] Skip atmosphere beyond `lod.atmosphereMaxDist`
- [ ] Drive `innerRadiusMul` / `outerRadiusMul` / `sunIntensity` from each
      `planets/*.yaml:atmosphere` section

### Water

- [ ] `assets/shaders/water.glsl` (refraction, DUDV wave distortion, fog
      colour absorption)
- [ ] `tools/gen_water_maps.py` — bake DUDV + normal map PNGs under
      `assets/textures/water/`
- [ ] Per-planet water plane gated by `planet.yaml:oceanLevel0`; fog
      colour/density already parsed into `planet_full_config_t`

## M3 — Star map mode

- [ ] Dedicated `STAR_MAP` mode with its own camera (pan + zoom, no orbit)
- [ ] Star catalogue (procedural seeded list, name labels)
- [ ] Picking + transition into a system's SOLAR view
- [ ] Labels rendered with `sokol_debugtext` or a glyph atlas

## M4 — RTS simulation (largest chunk; will need sub-slicing)

The C# reference's RTS sim runs on the Goldberg cell graph of a single
planet. Most of the engine.yaml knobs in `rtsCamera` / `slopes` /
`unitArrival` / `unitMovement` already document the target shape.

### Sim core

- [ ] `src/sim/` directory + cell-graph data (adjacency, level, slope flag,
      occupancy)
- [ ] Unit data: id, type, pos (cell + sub-slot), facing, hp, hitbox half-width
- [ ] `rts.yaml` schema mirroring upstream (unit types, per-cell capacity,
      speeds, attack stats)
- [ ] A* on cell graph; cliff-blocking via `slopesTraversable`/`canHop`
- [ ] Sub-cell slot allocator with min chord =
      `unitArrival.slotSpacingMultiplier × half-width`
- [ ] Altitude follow: lerp at `unitMovement.altitudeLerpRate` toward
      current cell's surface height
- [ ] Combat resolution (attack, damage, death)

### RTS camera mode

- [ ] `RTS_GROUND` mode: ground-clearance floor
      (`rtsCamera.groundClearance`), max tilt
      (`rtsCamera.maxTiltDegrees`)
- [ ] Zoom-driven tilt blend between
      `tiltStartZoomPercent` and `tiltFullZoomPercent`
- [ ] Smooth zoom via `rtsCamera.zoomLerpRate`, increment from
      `rtsCamera.scrollIncrement` (pinch parity required)
- [ ] Auto-switch PLANET_EDIT ↔ RTS_GROUND on altitude crossing

### RTS UI

- [ ] Selection box (drag rect) + tap-to-select (touch parity)
- [ ] Command palette: move, attack, stop, hold (on-screen buttons for touch)
- [ ] Selected-unit info panel, health bars
- [ ] Path debug overlay gated by `engine.yaml:debug.showUnitPaths`

## M5 — Shell / chrome / persistence

- [ ] Main menu (start, settings, quit)
- [ ] Settings dialog (resolution / vsync / volume) writing to a user
      config file
- [ ] Save / load (game state serialization — units, planet seed, camera)
- [ ] Audio subsystem: BGM + UI sfx (sokol_audio is the natural pick;
      pixel-art aesthetic suggests chiptune)

## M6 — Asset pipeline gaps

- [ ] `.obj` loader for "baked model" requests (procedural by default per
      CLAUDE.md)
- [ ] Animation: `*.anim.json` loader + `tools/gen_*_c.py` bakers for
      keyframed transforms
- [ ] Font glyph atlas baker (`tools/gen_font_atlas.py`) if/when we move
      off `sokol_debugtext`
- [ ] CI gate: regenerate sokol-shdc outputs and fail on drift (already
      promised in `CLAUDE.md`)

## Not in scope (parity = upstream parity)

- Multiplayer / networking — upstream has no implementation, so neither
  do we
- Mod loader — upstream YAML configs already cover "designer tweakable",
  which is the only modding hook so far

## Cross-cutting reminders

- Every new tunable goes through the four-step checklist in CLAUDE.md
  (struct field → YAML schema doc → parser → consumer). No magic
  numbers in C / GLSL.
- Touch parity in the **same commit** as any new desktop interaction.
- Generated assets are pixel-art (≤64×64, palette-limited PNGs under
  `assets/textures/`).
- Shaders authored once in GLSL, cross-compiled with sokol-shdc;
  generated headers under `src/gen/` are committed.
