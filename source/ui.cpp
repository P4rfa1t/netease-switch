#include "ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "util.hpp"

namespace ntv {

uint32_t rgba(int r, int g, int b, int a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static SDL_Color toColor(uint32_t c) {
    SDL_Color color{};
    color.a = (Uint8)((c >> 24) & 0xFF);
    color.r = (Uint8)((c >> 16) & 0xFF);
    color.g = (Uint8)((c >> 8) & 0xFF);
    color.b = (Uint8)(c & 0xFF);
    return color;
}

bool Ui::init(SDL_Renderer *renderer) {
    renderer_ = renderer;
    if (TTF_Init() != 0) {
        logf("[ui] TTF_Init failed: %s", TTF_GetError());
        return false;
    }

    Result rc = plGetSharedFontByType(&font_data_, PlSharedFontType_ChineseSimplified);
    if (R_FAILED(rc)) rc = plGetSharedFontByType(&font_data_, PlSharedFontType_Standard);
    if (R_FAILED(rc)) {
        logf("[ui] shared font unavailable: 0x%x", rc);
        return false;
    }
    has_font_ = true;
    logf("[ui] shared font loaded, %u bytes", font_data_.size);
    return true;
}

void Ui::shutdown() {
    for (auto &kv : cache_) SDL_DestroyTexture(kv.second);
    cache_.clear();
    for (auto &kv : fonts_) TTF_CloseFont(kv.second);
    fonts_.clear();
    TTF_Quit();
}

TTF_Font *Ui::font(int size) {
    auto it = fonts_.find(size);
    if (it != fonts_.end()) return it->second;
    if (!has_font_) return nullptr;

    SDL_RWops *rw = SDL_RWFromConstMem(font_data_.address, (int)font_data_.size);
    TTF_Font *f = TTF_OpenFontRW(rw, 1, size);
    if (!f) logf("[ui] TTF_OpenFontRW(%d) failed: %s", size, TTF_GetError());
    fonts_[size] = f;
    return f;
}

SDL_Texture *Ui::texture(const std::string &value, int size, uint32_t color) {
    char keybuf[64];
    snprintf(keybuf, sizeof(keybuf), "%d:%08X:", size, color);
    std::string key = keybuf + value;

    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    TTF_Font *f = font(size);
    if (!f) return nullptr;

    SDL_Surface *surface = TTF_RenderUTF8_Blended(f, value.c_str(), toColor(color));
    if (!surface) return nullptr;

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_FreeSurface(surface);

    if (cache_.size() > 600) {
        for (auto &kv : cache_) SDL_DestroyTexture(kv.second);
        cache_.clear();
    }
    cache_[key] = tex;
    return tex;
}

void Ui::clear(uint32_t color) {
    SDL_Color c = toColor(color);
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    SDL_RenderClear(renderer_);
}

void Ui::rect(int x, int y, int w, int h, uint32_t color) {
    SDL_Color c = toColor(color);
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(renderer_, &r);
}

void Ui::rectOutline(int x, int y, int w, int h, uint32_t color, int thickness) {
    SDL_Color c = toColor(color);
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    for (int i = 0; i < thickness; ++i) {
        SDL_Rect r{x + i, y + i, w - i * 2, h - i * 2};
        SDL_RenderDrawRect(renderer_, &r);
    }
}

void Ui::text(int x, int y, const std::string &value, int size, uint32_t color) {
    if (value.empty()) return;
    SDL_Texture *tex = texture(value, size, color);
    if (!tex) return;
    int w = 0, h = 0;
    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(renderer_, tex, nullptr, &dst);
}

void Ui::textRight(int right, int y, const std::string &value, int size, uint32_t color) {
    text(right - textWidth(value, size), y, value, size, color);
}

void Ui::textCentered(int cx, int y, const std::string &value, int size, uint32_t color) {
    text(cx - textWidth(value, size) / 2, y, value, size, color);
}

int Ui::textWidth(const std::string &value, int size) {
    TTF_Font *f = font(size);
    if (!f || value.empty()) return 0;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(f, value.c_str(), &w, &h) != 0) return (int)value.size() * size / 2;
    return w;
}

void Ui::textWrapped(int x, int y, int width, const std::string &value, int size, uint32_t color,
                     int line_height) {
    std::string line;
    size_t i = 0;
    int cursor_y = y;

    while (i < value.size()) {
        unsigned char c = (unsigned char)value[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > value.size()) len = 1;

        std::string ch = value.substr(i, len);
        i += len;

        if (ch == "\n") {
            text(x, cursor_y, line, size, color);
            cursor_y += line_height;
            line.clear();
            continue;
        }

        std::string candidate = line + ch;
        if (!line.empty() && textWidth(candidate, size) > width) {
            text(x, cursor_y, line, size, color);
            cursor_y += line_height;
            line = ch;
        } else {
            line = candidate;
        }
    }

    if (!line.empty()) text(x, cursor_y, line, size, color);
}

} // namespace ntv
