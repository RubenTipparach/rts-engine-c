/* sokol_impl.c — the *only* TU that defines SOKOL_IMPL.
 *
 * Sokol's single-header impl blocks are not idempotent across multiple
 * inclusions in the same TU, so SOKOL_IMPL must be set exactly once in
 * the program — here. Every other TU in the project includes the same
 * sokol_*.h headers as plain declarations.
 *
 * On macOS this file is compiled with `-x objective-c -fobjc-arc` so
 * sokol_app.h's Cocoa/Metal backend can build (see Makefile). On
 * Linux/Windows it compiles as C. */
/* HandmadeMath ships in the same single-header style: declarations
 * everywhere, implementation gated by HANDMADE_MATH_IMPLEMENTATION
 * in exactly one TU. Co-locate it here for the same reason.
 *
 * It MUST come before sokol_app.h on Linux: sokol_app.h pulls in
 * <X11/Xmd.h>, which `#define B32` (and other short bitfield names)
 * to nothing. HandmadeMath uses `B32` and friends as locals inside
 * matrix-inverse functions, so seeing the X11 macros first turns
 * those tokens into syntax errors. Including HandmadeMath first
 * means its inline function bodies tokenize while the macros are
 * still inert. */
#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"
#include "sokol_debugtext.h"
