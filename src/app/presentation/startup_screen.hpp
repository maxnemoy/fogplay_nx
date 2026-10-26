#pragma once
#include <SDL.h>
#include <SDL_ttf.h>
namespace fog::ui {
void drawStartupScreen(SDL_Renderer* renderer, TTF_Font* font, TTF_Font* titleFont, const char* message);
}
