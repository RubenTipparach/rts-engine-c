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
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"
