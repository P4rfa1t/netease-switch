#pragma once

#include <cstdint>
#include <map>
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

enum class Screen { Login, Home, Playlist, Player };
enum class Page { Recommend, Discover, Mine, Search };
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
    bool pressedRepeat(uint64_t button);

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
    void drawMineGrid();
    std::vector<Playlist> mineSectionPlaylists();
    void drawPlaylist();
    void drawNowBar();
    void drawLyrics();
    void drawToast();
    void drawTrackRow(const Song &song, int index, int y, bool selected);
    void drawScrollbar(int x, int y, int height, int visible, int total, int first);

    // ---- TV layout ----
    void drawTopBar();
    void drawPlaylistGrid(const std::vector<Playlist> &cards, int selected, int scroll,
                          int top, const char *empty_hint);
    void drawPlayerPage();
    void drawCover(const std::string &url, int x, int y, int w, int h);
    SDL_Texture *iconTexture(const char *name);
    void drawIcon(const char *name, int x, int y, int w, int h);
    void requestCover(const std::string &url);
    void pollCovers();
    void switchPage(Page page);
    void activateCard();
    void loadDiscover();
    int cardColumns() const { return 5; }
    int cardWidth() const { return 220; }
    int cardHeight() const { return 220; }

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
    // One timer per button: sharing a timer between Up/Left makes a single press
    // advance several steps because both branches see the same "fire" moment.
    double repeat_next_[64] = {};
    double repeat_next(HidNpadButton button) const {
        int i = 0;
        for (uint64_t b = button; b > 1; b >>= 1) ++i;
        return (i >= 0 && i < 64) ? repeat_next_[i] : 0.0;
    }
    void setRepeatNext(HidNpadButton button, double value) {
        int i = 0;
        for (uint64_t b = button; b > 1; b >>= 1) ++i;
        if (i >= 0 && i < 64) repeat_next_[i] = value;
    }

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
    SDL_Texture *avatar_ = nullptr;
    std::string avatar_url_;
    std::string vip_text_;
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
    std::string tracks_cover_;
    int track_sel_ = 0;
    int track_scroll_ = 0;
    int playing_index_ = -1;
    std::vector<Song> play_queue_;

    // ---- top tab bar + card grid (TV layout) ----
    Page page_ = Page::Recommend;
    int nav_index_ = 0;              // focused top tab
    int card_index_ = 0;             // focused card in the grid
    int card_scroll_ = 0;
    bool nav_focused_ = true;
    std::set<std::string> pending_covers_;
    std::map<std::string, SDL_Texture *> icons_;
    std::vector<Playlist> discover_playlists_;
    std::vector<HomeBlock> home_blocks_;
    int discover_sel_ = 0;
    int strip_scroll_ = 0;
    int discover_scroll_ = 0;
    std::vector<Playlist> mine_playlists_;   // for the "我的歌单" grid
    int mine_sel_ = 0;
    int mine_section_ = 0;      // 0 = 创建的歌单, 1 = 收藏的歌单
    int mine_focus_ = 0;
    std::vector<Playlist> collected_;
    bool collected_loaded_ = false;        // 0 = section tabs, 1 = grid
    bool show_playlist_grid_ = false;
    bool show_toplists_ = false;
    int mine_scroll_ = 0;
    uint64_t viewed_playlist_id_ = 0;

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
