// Native versions of the engine's hottest small pieces (see native.c).
#pragma once

#include "quickjs.h"

/** Defines globalThis.__native; call before evaluating engine.js. */
void native_install(JSContext *ctx);
