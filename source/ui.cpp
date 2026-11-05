#include "ui.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cmath>
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
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
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
    for (auto &kv : images_) SDL_DestroyTexture(kv.second);
    images_.clear();
    IMG_Quit();
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

void Ui::circle(int cx, int cy, int r, uint32_t color) {
    SDL_Color c = toColor(color);
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    for (int dy = -r; dy <= r; ++dy) {
        int dx = (int)std::sqrt((double)(r * r - dy * dy));
        SDL_RenderDrawLine(renderer_, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

void Ui::ring(int cx, int cy, int r, int thickness, uint32_t color) {
    circle(cx, cy, r, color);
    // Punch the middle out by painting the backing colour is not possible here,
    // so draw the ring as two arcs of horizontal spans instead.
    SDL_Color c = toColor(color);
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    int inner = r - thickness;
    if (inner <= 0) return;
    // redraw inner region in transparent-ish way is impossible; caller draws on
    // a known background, so we approximate with an outline of the inner circle
    for (int dy = -inner; dy <= inner; ++dy) {
        int dx = (int)std::sqrt((double)(inner * inner - dy * dy));
        SDL_RenderDrawPoint(renderer_, cx - dx, cy + dy);
        SDL_RenderDrawPoint(renderer_, cx + dx, cy + dy);
    }
}

void Ui::triangle(int x, int y, int w, int h, const char *dir, uint32_t color) {
    SDL_Color c = toColor(color);
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    bool right = dir[0] == 'r';
    bool left = dir[0] == 'l';
    bool up = dir[0] == 'u';

    if (left || right) {
        for (int i = 0; i < w; ++i) {
            int half = (h / 2) * (w - i) / (w > 0 ? w : 1);
            int cx = right ? x + i : x + w - i;
            SDL_RenderDrawLine(renderer_, cx, y + h / 2 - half, cx, y + h / 2 + half);
        }
    } else {
        for (int i = 0; i < h; ++i) {
            int half = (w / 2) * (h - i) / (h > 0 ? h : 1);
            int cy = up ? y + i : y + h - i;
            SDL_RenderDrawLine(renderer_, x + w / 2 - half, cy, x + w / 2 + half, cy);
        }
    }
}

void Ui::pauseBars(int x, int y, int w, int h, uint32_t color) {
    int bar = w / 3;
    rect(x, y, bar, h, color);
    rect(x + w - bar, y, bar, h, color);
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

SDL_Texture *Ui::cachedImage(const std::string &key) {
    auto it = images_.find(key);
    return it == images_.end() ? nullptr : it->second;
}

SDL_Texture *Ui::textureFromMemory(const std::string &key,
                                   const std::vector<unsigned char> &data) {
    SDL_RWops *rw = SDL_RWFromConstMem(data.data(), (int)data.size());
    if (!rw) return nullptr;
    SDL_Surface *surface = IMG_Load_RW(rw, 1);
    if (!surface) {
        logf("[ui] image decode failed: %s", IMG_GetError());
        return nullptr;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_FreeSurface(surface);
    if (!tex) return nullptr;

    if (images_.size() > 30) {
        for (auto &kv : images_) SDL_DestroyTexture(kv.second);
        images_.clear();
    }
    images_[key] = tex;
    return tex;
}

void Ui::image(SDL_Texture *texture, int x, int y, int w, int h) {
    if (!texture) return;
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(renderer_, texture, nullptr, &dst);
}

} // namespace ntv
