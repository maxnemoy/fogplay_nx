#include "core/constants/ui_text.hpp"
#include "app/presentation/startup_screen.hpp"
#include <memory>
#include <stdexcept>

namespace fog::ui {
namespace {
void text(SDL_Renderer* renderer, TTF_Font* font, const char* message, int y, SDL_Color color) {
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(
        TTF_RenderUTF8_Blended(font,message,color), SDL_FreeSurface);
    if (!surface) throw std::runtime_error(TTF_GetError());
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
        SDL_CreateTextureFromSurface(renderer,surface.get()), SDL_DestroyTexture);
    if (!texture) throw std::runtime_error(SDL_GetError());
    const SDL_Rect rect{(1280-surface->w)/2,y,surface->w,surface->h};
    if (SDL_RenderCopy(renderer,texture.get(),nullptr,&rect) != 0) throw std::runtime_error(SDL_GetError());
}
}
void drawStartupScreen(SDL_Renderer* renderer, TTF_Font* font, TTF_Font* titleFont, const char* message) {
    SDL_SetRenderDrawColor(renderer,12,14,29,255);
    if (SDL_RenderClear(renderer) != 0) throw std::runtime_error(SDL_GetError());
    SDL_SetRenderDrawColor(renderer,174,131,245,255);
    const SDL_Rect accent{592,243,96,5};
    SDL_RenderFillRect(renderer,&accent);
    text(renderer,titleFont,fog::constants::ui::Common::BrandCaption,278,{247,248,255,255});
    text(renderer,font,message,350,{183,188,210,255});
    SDL_RenderPresent(renderer);
}
}
