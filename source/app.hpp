#pragma once

#include <cstdint>
#include <set>
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
enum class PlayMode { Sequential, RepeatOne, Shuffle };

struct LyricLine {
    double time = 0;
    std::string text;
    std::string translation;
};

class App {
public:
    int run();

private:
    bool init();
    void shutdown();
    void loopFrame();

    void pollInput();
    void pollTouch();
    void handleTap(float x, float y);
    void handleDrag(float dx, float dy);
    bool held(uint64_t button) const { return (held_ & button) != 0; }
    bool pressed(uint64_t button) const { return (down_ & button) != 0; }
    bool pressedRepeat(uint64_t button, double &next_time);

    void update();
    void draw();
    void redrawNow();

    void startLogin();
    void pollLogin();
    void waitForNetwork();
    bool loadAccount();
    void loadHome();
    void refreshHomePage();
    void loadRecent();
    void loadToplists();
    void loadRecommend();
    void ensureTabLoaded();
    void ensureLikedIds();
    void requestLogout();
    void logout();
    void toggleSidebar();
    int sidebarWidth() const;
    int contentX() const;
    int contentWidth() const;
    void loadCover(const Song &song);
    void cycleQuality();
    void toggleLikeCurrent();
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
    PadState pad_{};
    bool running_ = true;

    HidTouchScreenState touch_state_{};
    bool touch_logged_ = false;
    bool touch_active_ = false;
    bool touch_dragged_ = false;
    float touch_last_x_ = 0;
    float touch_last_y_ = 0;
    float touch_start_x_ = 0;
    float touch_start_y_ = 0;
    float touch_move_ = 0;
    float drag_accum_ = 0;
    double touch_start_time_ = 0;

    uint64_t down_ = 0;
    uint64_t held_ = 0;
    double repeat_up_next_ = 0;
    double repeat_down_next_ = 0;
    double repeat_vol_up_next_ = 0;
    double repeat_vol_down_next_ = 0;

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
    std::vector<Song> recent_;
    std::vector<Playlist> toplists_;
    std::vector<Playlist> recommend_;
    int rec_sel_ = 0;
    int rec_scroll_ = 0;
    uint64_t liked_playlist_id_ = 0;
    int liked_track_count_ = 0;
    int top_sel_ = 0;
    int top_scroll_ = 0;
    std::set<uint64_t> liked_ids_;
    bool liked_ids_loaded_ = false;
    uint64_t current_song_id_ = 0;
    SDL_Texture *cover_ = nullptr;
    std::string cover_key_;
    int quality_index_ = 2;
    int daily_sel_ = 0;
    int daily_scroll_ = 0;
    int playlist_sel_ = 0;
    int playlist_scroll_ = 0;
    int recent_sel_ = 0;
    int recent_scroll_ = 0;

    std::vector<Song> tracks_;
    std::string tracks_title_;
    int track_sel_ = 0;
    int track_scroll_ = 0;
    int playing_index_ = -1;
    std::vector<Song> play_queue_;

    PlayMode play_mode_ = PlayMode::Sequential;
    bool sidebar_collapsed_ = false;
    bool sidebar_focus_ = false;
    bool confirm_logout_ = false;
    double logout_confirm_until_ = 0;
    bool show_lyrics_ = false;
    uint64_t lyrics_song_ = 0;
    std::vector<LyricLine> lyrics_;

    std::string toast_text_;
    double toast_until_ = 0;
    double auto_next_at_ = 0;
    double refresh_allowed_at_ = 0;
    std::string last_player_error_;
};

} // namespace ntv
