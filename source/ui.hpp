#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>

namespace ntv {

uint32_t rgba(int r, int g, int b, int a = 255);

class Ui {
public:
    bool init(SDL_Renderer *renderer);
    void shutdown();

    void clear(uint32_t color);
    void rect(int x, int y, int w, int h, uint32_t color);
    void rectOutline(int x, int y, int w, int h, uint32_t color, int thickness = 2);

    void text(int x, int y, const std::string &value, int size, uint32_t color);
    void textRight(int right, int y, const std::string &value, int size, uint32_t color);
    void textCentered(int cx, int y, const std::string &value, int size, uint32_t color);
    void textWrapped(int x, int y, int width, const std::string &value, int size, uint32_t color,
                     int line_height);
    int textWidth(const std::string &value, int size);

private:
    TTF_Font *font(int size);
    SDL_Texture *texture(const std::string &value, int size, uint32_t color);

    SDL_Renderer *renderer_ = nullptr;
    PlFontData font_data_{};
    bool has_font_ = false;
    std::map<int, TTF_Font *> fonts_;
    std::map<std::string, SDL_Texture *> cache_;
};

} // namespace ntv
