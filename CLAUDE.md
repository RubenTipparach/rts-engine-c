# Repo rules

## Reference implementation is read-only

The original C# / Silk.NET / Blazor WASM RTS engine is vendored as a git
submodule at `reference/rts-engine/`. It is the reference implementation
for everything we're porting to C/Sokol — solar system, planet mesh,
star map, atmosphere, water, UI, RTS sim.

**Do not edit, refactor, or "fix" any file under `reference/rts-engine/`** —
including shaders under `reference/rts-engine/assets/shaders/`, configs
under `reference/rts-engine/assets/config/`, planet YAMLs, scene/mode
files, or anything else. Reading the C# to learn how a feature works is
fine and encouraged. Touching it is not.

If something feels like it should change in the C# reference, instead
change the C port to no longer depend on the C# shape — or surface the
question and let the user decide.

This rule stays in force until the user says otherwise.

## Active port lives at the repo root

All development happens in C + Sokol at the repo root. Layout:

- `src/` — C engine code (core, renderer, sim, etc.)
- `assets/` — runtime content (configs, shaders, planet YAMLs, models, animations)
- `third_party/` — vendored single-header libs (sokol_*.h, stb_*.h, etc.)
- `tools/` — build-time generators (texture/animation bakers)
- `reference/rts-engine/` — read-only C# reference (submodule)

The build produces two targets: native desktop (sokol_app + native GL/
Metal/D3D11) and web (sokol_app + WebGL2 via emscripten).

## Tunable values live in `assets/config/*.yaml` and `assets/planets/*.yaml`

Anything that's "a knob a designer or modder might want to turn without
recompiling" lives in YAML under `assets/`, not as a `const` or `#define`
in C/GLSL. The reference repo already has the schemas — we mirror them
where possible so YAMLs can be copy-pasted between projects.

Conventions:

- One config file per subsystem (`solarsystem.yaml`, `engine.yaml`,
  `rts.yaml`, etc.). Per-body files under `planets/*.yaml`. Keep the
  schema documented at the top of the YAML itself so the file is
  self-describing.
- Sections are body / object names; fields are lowercase
  underscore_case (the C# reference uses camelCase — the C YAML parser
  accepts both via case-insensitive match).
- A field omitted entirely or set to its zero value means "use the
  compiled-in default". The C side merges by checking `> 0.0f` per
  field — this lets the YAML express partial overrides.
- The parser is a deliberately tiny YAML subset (key/value, vec3
  lists, top-level sections, nested lists for moons, `#` comments).
  Don't reach for libyaml for a handful of config files.
- Files are bundled via emscripten's `--preload-file` for web builds,
  so they ship with the WASM binary at build time. Native builds read
  them off disk at boot for true edit-and-rerun iteration.

When adding a new tunable: (1) add field to the relevant struct in
`src/core/`, (2) extend the YAML schema doc-comment, (3) extend the
parser's switch in `src/core/config.c`, (4) plumb through to the
consumer (renderer / sim / whatever).

## No magic numbers in code

Unless a value is a true mathematical or protocol constant that will
never need to change (π, the speed of light, the on-disk header magic
of a fixed file format), **do not hard-code numeric literals in
C/GLSL**. Every "designer-tweakable" or "feels-right-after-iteration"
number — colours, sizes, thresholds, falloff curves, density layers,
phase asymmetries, drift speeds, intensities — belongs in a YAML file
under `assets/` and reaches the consumer via a `config_*()` getter.

If you find yourself writing `* 0.85` or `vec3(0.55, 0.18, 0.38)` in
a shader or sim path, stop. Add a field to the relevant
`*_config_t`, extend the YAML schema and parser, and read it as a
uniform / value at runtime instead. The four-step checklist above is
the same one you'd follow for any new tunable.

The only exception is values that are part of an algorithm's
*identity* — e.g., the magic constants inside a hash function
(`0.1031`, `443.897`) where the specific number doesn't have a
semantic meaning, it just makes the hash work. Those stay inline,
because exposing them would invite breakage rather than tuning.

## Visual aesthetic — pixel art

The intended look of this project is **pixel art**: chunky, low-res,
deliberately limited palettes, visible stair-stepping over smooth
gradients. Every visual choice should lean into that — palette-driven
flat colours, hard edges between regions, dithered transitions
instead of antialiased ones, no soft fades or large gradient washes
that read as "modern realistic."

Design implications:

- Texture *resolution* is bounded — see the texture rule below.
- Procedural meshes and shaders should prefer stepped output over
  smooth: discrete biome cells are right, vertex-interpolated
  gradient blends across cells are wrong. Bayer dithering is fine
  (it's the pixel-art way to fade an edge).
- When picking colours, use the per-planet YAML palettes literally —
  don't add post-process exposure / saturation passes that wash them
  toward "filmic." If a planet's biome list says
  `[0.30, 0.65, 0.25]`, that's the green you want on screen.
- New shaders that exist purely for "looks better" with no pixel-art
  affordance (heavy bloom, screen-space AO, motion blur) should be
  avoided unless they have a pixel-friendly form (e.g. a quantised
  bloom that snaps to N steps).

## Static game asset guidelines

These rules apply to every asset Claude generates, hand-edits, or asks
the user to drop into the repo. Keep them in mind whenever a task
involves "generate a model / texture / animation".

- **Textures** — if the task involves generating textures, the result
  must be baked to PNG (8-bit RGBA, **≤64×64**) and committed under
  `assets/textures/`. Anything bigger reads as "modern texture art"
  and breaks the pixel-art aesthetic. Most use cases (terrain
  patches, UI sprites, particle masks) are well-served by 16×16 or
  32×32; reserve 64×64 for cases that genuinely need the resolution
  (e.g. a font atlas or a hero sprite). Build-time conversions
  (e.g., to `.sprite` or compressed formats) are derived artifacts —
  do not commit them, only the source PNG.
- **3D models** — this is a procedural game, so the default for new 3D
  content is to generate it procedurally in code (mesh built at runtime
  from parameters, the way planets / orbit rings / starfields already
  work). Only when the user explicitly asks for a **"custom model"** or
  **"baked model"** do we go to the baked-asset path: a `.obj` file
  under `assets/models/`, optionally with a matching JSON sidecar for
  the in-browser model editor, with the `.obj` as the source of truth.
  Generators for baked models live under `tools/` and write both.
- **Animations** — if the task involves animation (walk cycles,
  machinery motion, scripted camera moves, anything that sweeps a
  transform over time), the keyframes must be baked to an animation
  file under `assets/animations/`. The current pipeline uses simple
  JSON `*.anim.json` (frames + per-bone transforms) plus an optional
  baked C header generated by a `tools/gen-*-c.py` script. Procedural
  per-frame tweaks layered on top of the baked clip are fine; the base
  motion must come from the file.
- **Generation scripts** go under `tools/`. Each generator follows the
  pattern of the reference repo's `tools/gen_*.py`: a Python entry
  point that writes out the baked artifact deterministically, with no
  third-party deps beyond stdlib + Pillow + numpy + zlib.

## Sokol & shader pipeline

- Use **sokol_app + sokol_gfx + sokol_glue** as single-header libs
  under `third_party/sokol/`. Pinned to specific commits; bumped
  deliberately, not with `git submodule update --remote`.
- Shaders are authored as GLSL once under `assets/shaders/*.glsl` and
  cross-compiled with **sokol-shdc** to GLSL330 (desktop GL), GLES3
  (WebGL2), Metal, and HLSL — outputs go to a generated header per
  shader (e.g. `src/gen/solarsystem.glsl.h`) which the C code includes.
  Generated headers are committed so a clean clone builds without
  needing `sokol-shdc` installed locally; CI regenerates them and
  fails on drift.
- Backend selection: native picks the platform default (GL on Linux,
  Metal on macOS, D3D11 on Windows). Web is fixed to WebGL2 via
  emscripten.

## Open questions → AskUserQuestion / plan mode

If a response would otherwise end with open questions or pending
decisions back to the user, surface them through the
`AskUserQuestion` tool (or `ExitPlanMode` if you're presenting a
plan to approve) instead of trailing prose questions. Clickable
choices are faster to answer than retyping prose, and grouping the
questions keeps the conversation tight. Only fall back to inline
prose questions when the answer space is genuinely free-form (a name,
a path, a numeric tolerance) rather than a pick from a small set.

## Post-commit links

After each commit, always show these links to the user:

- Play the game (web build): https://rubentipparach.github.io/rts-engine-c/
- Track CI builds: https://github.com/RubenTipparach/rts-engine-c/actions
