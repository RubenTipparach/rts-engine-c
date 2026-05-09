#pragma once

#include <stdio.h>

/* INFO lines go to stdout — under emscripten that means
 * Module.print -> console.log, which keeps them out of the dev-tools
 * "error" stream and out of the on-page error tray. WARN and ERROR
 * stay on stderr (Module.printErr), which the shell still surfaces
 * as a real error. */
#define LOG_INFO(fmt, ...)  fprintf(stdout, "[info]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[warn]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[error] " fmt "\n", ##__VA_ARGS__)
