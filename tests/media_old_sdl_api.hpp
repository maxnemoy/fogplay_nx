#pragma once
// Exercise production's unavailable-API branch against the installed SDL.
// This is compile-guard coverage, not compatibility with an old SDL binary.
#include <SDL.h>
#undef SDL_VERSION_ATLEAST
#define SDL_VERSION_ATLEAST(X, Y, Z) 0
