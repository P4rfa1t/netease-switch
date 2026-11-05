#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
    void circle(int cx, int cy, int r, uint32_t color);
    void ring(int cx, int cy, int r, int thickness, uint32_t color);
    // Filled triangle; dir = "up" | "down" | "left" | "right"
    void triangle(int x, int y, int w, int h, const char *dir, uint32_t color);
    // Two vertical bars (pause icon)
    void pauseBars(int x, int y, int w, int h, uint32_t color);

    void text(int x, int y, const std::string &value, int size, uint32_t color);
    void textRight(int right, int y, const std::string &value, int size, uint32_t color);
    void textCentered(int cx, int y, const std::string &value, int size, uint32_t color);
    void textWrapped(int x, int y, int width, const std::string &value, int size, uint32_t color,
                     int line_height);
    int textWidth(const std::string &value, int size);

    SDL_Texture *cachedImage(const std::string &key);
    SDL_Texture *textureFromMemory(const std::string &key,
                                   const std::vector<unsigned char> &data);
    void image(SDL_Texture *texture, int x, int y, int w, int h);

private:
    TTF_Font *font(int size);
    SDL_Texture *texture(const std::string &value, int size, uint32_t color);

    SDL_Renderer *renderer_ = nullptr;
    PlFontData font_data_{};
    bool has_font_ = false;
    std::map<int, TTF_Font *> fonts_;
    std::map<std::string, SDL_Texture *> cache_;
    std::map<std::string, SDL_Texture *> images_;
};

} // namespace ntv
