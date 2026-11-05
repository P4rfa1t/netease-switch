#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <switch.h>

#include "api.hpp"
#include "audio.hpp"
#include "http.hpp"
#include "ui.hpp"

namespace ntv {

enum class Screen { Login, Home, Playlist };

struct LyricLine {
    double time = 0;
    std::string text;
};

class App {
public:
    int run();

private:
    bool init();
    void shutdown();
    void loopFrame();

    void pollInput();
    bool held(SDL_GameControllerButton button) const { return current_[button]; }
    bool pressed(SDL_GameControllerButton button) const {
        return current_[button] && !previous_[button];
    }
    bool pressedRepeat(SDL_GameControllerButton button);

    void update();
    void draw();
    void redrawNow();

    void startLogin();
    void pollLogin();
    void waitForNetwork();
    bool loadAccount();
    void loadHome();
    void refreshHomePage();
    void openSelected();
    void openPlaylist(uint64_t id, const std::string &name);
    void playSelectedFromPlaylist();
    void playSong(const Song &song);
    void playAdjacent(int direction);
    void startSearch();
    void loadLyrics(uint64_t song_id);
    void toast(const std::string &message);

    void drawLogin();
    void drawHome();
    void drawPlaylist();
    void drawSidebar();
    void drawNowBar();
    void drawLyrics();
    void drawToast();
    void drawTrackRow(const Song &song, int index, int y, bool selected);
    void drawScrollbar(int x, int y, int height, int visible, int total, int first);

    HttpClient http_;
    NetEaseApi api_{http_};
    Player player_;
    Ui ui_;

    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_GameController *controller_ = nullptr;
    bool running_ = true;

    bool current_[SDL_CONTROLLER_BUTTON_MAX]{};
    bool previous_[SDL_CONTROLLER_BUTTON_MAX]{};
    double repeat_next_[SDL_CONTROLLER_BUTTON_MAX]{};
    double trigger_cooldown_ = 0;

    Screen screen_ = Screen::Login;

    uint64_t uid_ = 0;
    std::string nickname_;

    std::string net_error_;
    std::string qr_key_;
    std::string qr_url_;
    int qr_code_ = 0;
    double next_qr_poll_ = 0;

    int tab_ = 0;
    std::vector<Song> daily_;
    std::vector<Playlist> playlists_;
    int daily_sel_ = 0;
    int daily_scroll_ = 0;
    int playlist_sel_ = 0;
    int playlist_scroll_ = 0;

    std::vector<Song> tracks_;
    std::string tracks_title_;
    int track_sel_ = 0;
    int track_scroll_ = 0;
    int playing_index_ = -1;
    bool playing_playlist_ = false;

    bool show_lyrics_ = false;
    uint64_t lyrics_song_ = 0;
    std::vector<LyricLine> lyrics_;

    std::string toast_text_;
    double toast_until_ = 0;
    double auto_next_at_ = 0;
    std::string last_player_error_;
};

} // namespace ntv
