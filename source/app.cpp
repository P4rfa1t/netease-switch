#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <netdb.h>
#include <sys/socket.h>

#include <SDL2/SDL.h>

#include <switch/runtime/resolver.h>

#include "qrcodegen.hpp"
#include "util.hpp"

namespace ntv {

namespace {

const uint32_t kBg = rgba(18, 18, 22);
const uint32_t kSidebarBg = rgba(24, 24, 30);
const uint32_t kPanelBg = rgba(30, 30, 38);
const uint32_t kAccent = rgba(236, 65, 65);
const uint32_t kText = rgba(236, 236, 242);
const uint32_t kSub = rgba(146, 146, 158);
const uint32_t kRowSelected = rgba(236, 65, 65, 46);
const uint32_t kWhite = rgba(255, 255, 255);

constexpr int kSidebarWidth = 300;
constexpr int kNowBarY = 632;
constexpr int kRowHeight = 56;
constexpr int kVisibleRows = 9;
constexpr int kListTop = 88;

double clockSeconds() {
    return (double)SDL_GetTicks64() / 1000.0;
}

void keepVisible(int &scroll, int selected, int visible) {
    if (selected < scroll) scroll = selected;
    if (visible > 0 && selected >= scroll + visible) scroll = selected - visible + 1;
    if (scroll < 0) scroll = 0;
}

int dnsProbe(const char *host) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = nullptr;
    int rc = getaddrinfo(host, "443", &hints, &result);
    logf("[net] getaddrinfo(%s)=%d resolver=0x%x", host, rc, resolverGetLastResult());
    if (result) freeaddrinfo(result);
    return rc;
}

std::string dnsErrorMessage(int rc) {
    switch (rc) {
    case EAI_AGAIN: return "DNS 查询超时";
    case EAI_NONAME: return "域名解析失败";
    case EAI_SYSTEM: return "网络未连接";
    default: return "网络请求失败";
    }
}

} // namespace

int App::run() {
    if (!init()) return 1;

    while (running_ && appletMainLoop()) {
        pollInput();
        update();
        draw();
        SDL_RenderPresent(renderer_);
        SDL_Delay(15);
    }

    shutdown();
    return 0;
}

bool App::init() {
    romfsInit();
    plInitialize(PlServiceType_User);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        logf("[app] SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    if (SDL_CreateWindowAndRenderer(1280, 720, SDL_WINDOW_SHOWN, &window_, &renderer_) != 0) {
        logf("[app] window creation failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    logInit();
    Result socket_rc = socketInitializeDefault();
    logf("[net] socketInitializeDefault=0x%x", socket_rc);
    logf("[app] starting netease-switch");

    player_.init();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad_);
    hidInitializeTouchScreen();
    logf("[app] input: libnx pad + touch");

    if (!ui_.init(renderer_)) return false;

    waitForNetwork();

    http_.loadCookies("sdmc:/switch/netease-switch/cookies.json");
    http_.loadCookieText("sdmc:/switch/netease-switch/cookie.txt");
    if (api_.loggedIn() && loadAccount()) {
        loadHome();
    } else {
        startLogin();
    }
    return true;
}

void App::shutdown() {
    player_.shutdown();
    ui_.shutdown();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
    socketExit();
    plExit();
    romfsExit();
}

void App::waitForNetwork() {
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        logf("[net] nifmInitialize failed 0x%x", rc);
        return;
    }

    ui_.clear(kBg);
    ui_.textCentered(640, 320, "正在检查网络…", 24, kText);
    ui_.textCentered(640, 362, "请确认 Switch 已连接 Wi-Fi", 16, kSub);
    SDL_RenderPresent(renderer_);

    for (int i = 0; i < 12; ++i) {
        NifmInternetConnectionType type{};
        u32 strength = 0;
        NifmInternetConnectionStatus status{};
        Result st = nifmGetInternetConnectionStatus(&type, &strength, &status);
        if (R_SUCCEEDED(st)) {
            logf("[net] connection type=%d strength=%u status=%d", (int)type, strength, (int)status);
            if (status == NifmInternetConnectionStatus_Connected) break;
        } else {
            logf("[net] nifmGetInternetConnectionStatus failed 0x%x", st);
        }
        svcSleepThread(500000000ULL);
    }

    nifmExit();
}

void App::startLogin() {
    screen_ = Screen::Login;
    show_lyrics_ = false;
    lyrics_.clear();
    qr_key_.clear();
    qr_url_.clear();
    qr_code_ = 0;

    refresh_allowed_at_ = clockSeconds() + 5.0;

    if (!api_.qrCreate(qr_key_)) {
        int rc = dnsProbe("music.163.com");
        net_error_ = dnsErrorMessage(rc) + " (" + std::to_string(rc) + ")";
        toast(net_error_);
        return;
    }
    net_error_.clear();
    qr_url_ = "https://music.163.com/login?codekey=" + qr_key_;
    qr_code_ = 801;
    next_qr_poll_ = clockSeconds() + 2.0;
    logf("[login] qr created");
}

void App::pollLogin() {
    if (screen_ != Screen::Login || qr_key_.empty()) return;
    double now = clockSeconds();
    if (now < next_qr_poll_) return;
    next_qr_poll_ = now + 2.0;

    int code = api_.qrCheck(qr_key_);
    qr_code_ = code;
    logf("[login] poll code=%d", code);

    if (code == 0) {
        net_error_ = "网络异常，稍后自动重试";
    } else {
        net_error_.clear();
    }

    if (code == 803) {
        http_.saveCookies("sdmc:/switch/netease-switch/cookies.json");
        if (loadAccount()) {
            toast("登录成功");
            loadHome();
        } else {
            toast("登录状态获取失败");
            startLogin();
        }
    } else if (code == 800) {
        qr_key_.clear();
    } else if (code != 801 && code != 802) {
        logf("[login] unexpected code %d", code);
        qr_key_.clear();
    }
}

bool App::loadAccount() {
    return api_.account(uid_, nickname_);
}

void App::loadHome() {
    screen_ = Screen::Home;
    tab_ = 0;
    daily_.clear();
    playlists_.clear();
    daily_sel_ = daily_scroll_ = 0;
    playlist_sel_ = playlist_scroll_ = 0;

    bool ok = api_.dailySongs(daily_);
    ok = api_.userPlaylists(uid_, playlists_) || ok;
    if (!ok) toast("加载失败，按 + 重试");
}

void App::loadRecent() {
    redrawNow();
    std::vector<Song> songs;
    if (!api_.recentSongs(uid_, songs)) {
        toast("最近播放加载失败");
        return;
    }
    recent_ = songs;
    recent_sel_ = 0;
    recent_scroll_ = 0;
}

void App::refreshHomePage() {
    redrawNow();
    if (tab_ == 0) {
        std::vector<Song> songs;
        if (api_.dailySongs(songs)) {
            daily_ = songs;
            daily_sel_ = std::min(daily_sel_, (int)daily_.size() - 1);
            if (daily_sel_ < 0) daily_sel_ = 0;
            toast("已刷新");
        } else {
            toast("刷新失败");
        }
    } else if (tab_ == 2) {
        loadRecent();
        toast("已刷新");
    } else if (tab_ == 1) {
        std::vector<Playlist> lists;
        if (api_.userPlaylists(uid_, lists)) {
            playlists_ = lists;
            playlist_sel_ = std::min(playlist_sel_, (int)playlists_.size() - 1);
            if (playlist_sel_ < 0) playlist_sel_ = 0;
            toast("已刷新");
        } else {
            toast("刷新失败");
        }
    }
}

void App::openSelected() {
    if (tab_ == 0) {
        if (daily_.empty()) return;
        play_queue_ = daily_;
        playing_index_ = daily_sel_;
        playSong(daily_[daily_sel_]);
    } else if (tab_ == 1) {
        if (playlists_.empty()) return;
        const Playlist &p = playlists_[playlist_sel_];
        openPlaylist(p.id, p.name);
    } else if (tab_ == 2) {
        if (recent_.empty()) {
            loadRecent();
            return;
        }
        play_queue_ = recent_;
        playing_index_ = recent_sel_;
        playSong(recent_[recent_sel_]);
    } else {
        startSearch();
    }
}

void App::openPlaylist(uint64_t id, const std::string &name) {
    redrawNow();
    std::vector<Song> songs;
    std::string real_name;
    if (!api_.playlistDetail(id, songs, real_name)) {
        toast("歌单加载失败");
        return;
    }
    tracks_ = songs;
    tracks_title_ = real_name.empty() ? name : real_name;
    track_sel_ = 0;
    track_scroll_ = 0;
    screen_ = Screen::Playlist;
}

void App::playSelectedFromPlaylist() {
    if (tracks_.empty()) return;
    play_queue_ = tracks_;
    playing_index_ = track_sel_;
    playSong(tracks_[track_sel_]);
}

void App::playSong(const Song &song) {
    redrawNow();

    std::string url;
    std::string error;
    if (!api_.songUrl(song.id, url, error)) {
        toast(error);
        auto_next_at_ = clockSeconds() + 60.0;
        return;
    }

    player_.play(url, song.name, song.artist);
    loadLyrics(song.id);
    show_lyrics_ = false;
    last_player_error_.clear();
    auto_next_at_ = clockSeconds() + 2.0;
}

void App::playAdjacent(int direction) {
    if (play_queue_.empty()) return;

    int count = (int)play_queue_.size();
    if (playing_index_ < 0) playing_index_ = 0;
    playing_index_ = (playing_index_ + direction + count) % count;
    playSong(play_queue_[playing_index_]);
}

void App::startSearch() {
    SwkbdConfig config;
    if (R_FAILED(swkbdCreate(&config, 0))) return;
    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetGuideText(&config, "搜索歌曲或歌手");

    char input[256] = {};
    Result rc = swkbdShow(&config, input, sizeof(input));
    swkbdClose(&config);
    if (R_FAILED(rc) || input[0] == '\0') return;

    redrawNow();
    std::vector<Song> results;
    if (!api_.search(input, results)) {
        toast("没有找到结果");
        return;
    }
    tracks_ = results;
    tracks_title_ = std::string("搜索：") + input;
    track_sel_ = 0;
    track_scroll_ = 0;
    play_queue_ = tracks_;
    playing_index_ = -1;
    screen_ = Screen::Playlist;
}

void App::loadLyrics(uint64_t song_id) {
    lyrics_.clear();
    lyrics_song_ = song_id;

    std::string lrc;
    if (!api_.lyric(song_id, lrc)) return;

    size_t pos = 0;
    while (pos < lrc.size()) {
        size_t end = lrc.find('\n', pos);
        if (end == std::string::npos) end = lrc.size();
        std::string line = lrc.substr(pos, end - pos);
        pos = end + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

        double offset = 0.0;
        size_t cursor = 0;
        bool has_tag = false;
        while (cursor < line.size() && line[cursor] == '[') {
            size_t close = line.find(']', cursor);
            if (close == std::string::npos) break;
            std::string tag = line.substr(cursor + 1, close - cursor - 1);
            int minutes = 0;
            int seconds = 0;
            char dot = 0;
            char fraction[8] = {};
            if (sscanf(tag.c_str(), "%d:%d%c%7s", &minutes, &seconds, &dot, fraction) == 4 &&
                dot == '.') {
                double frac = 0.0;
                size_t flen = strlen(fraction);
                if (flen > 0) {
                    int value = atoi(fraction);
                    frac = value / std::pow(10.0, (double)flen);
                }
                offset = minutes * 60.0 + seconds + frac;
                has_tag = true;
                cursor = close + 1;
            } else {
                break;
            }
        }

        if (!has_tag) continue;
        std::string text = line.substr(cursor);
        if (text.empty()) continue;
        if (text[0] == '{') continue;
        lyrics_.push_back({offset, text});
    }

    std::sort(lyrics_.begin(), lyrics_.end(),
              [](const LyricLine &a, const LyricLine &b) { return a.time < b.time; });
    logf("[lyrics] song=%llu lines=%d", (unsigned long long)song_id, (int)lyrics_.size());
}

void App::toast(const std::string &message) {
    toast_text_ = message;
    toast_until_ = clockSeconds() + 3.0;
    logf("[toast] %s", message.c_str());
}

bool App::pressedRepeat(uint64_t button, double &next_time) {
    if (!(held_ & button)) {
        next_time = 0;
        return false;
    }
    double now = clockSeconds();
    if (down_ & button) {
        next_time = now + 0.35;
        return true;
    }
    if (now >= next_time) {
        next_time = now + 0.08;
        return true;
    }
    return false;
}

void App::pollTouch() {
    HidTouchScreenState state{};
    size_t count = hidGetTouchScreenStates(&state, 1);
    bool down = count > 0;

    if (down) {
        const HidTouchState &touch = state.touches[0];
        float x = (float)touch.x;
        float y = (float)touch.y;

        if (!touch_active_) {
            touch_active_ = true;
            touch_dragged_ = false;
            touch_move_ = 0;
            touch_last_x_ = x;
            touch_last_y_ = y;
            touch_start_y_ = y;
            touch_start_time_ = clockSeconds();
            drag_accum_ = 0;
        } else {
            float dx = x - touch_last_x_;
            float dy = y - touch_last_y_;
            touch_move_ += std::fabs(dx) + std::fabs(dy);
            if (touch_move_ > 16.0f) touch_dragged_ = true;
            if (touch_dragged_) handleDrag(dx, dy);
            touch_last_x_ = x;
            touch_last_y_ = y;
        }
    } else if (touch_active_) {
        touch_active_ = false;
        double duration = clockSeconds() - touch_start_time_;
        if (!touch_dragged_ && duration < 0.7) handleTap(touch_last_x_, touch_last_y_);
    }
}

void App::handleTap(float x, float y) {
    if (show_lyrics_) {
        show_lyrics_ = false;
        return;
    }

    if (y >= kNowBarY) {
        if (x >= 24 && x <= 112 && y >= 640) {
            player_.togglePause();
        } else if (x >= 420 && x <= 1100 && y >= 660 && y <= 700) {
            double total = player_.duration();
            if (total > 0) player_.seek(((x - 420.0) / 680.0) * total);
        } else if (x >= 1120 && y >= 640) {
            if (lyrics_.empty()) toast("当前歌曲暂无歌词");
            else show_lyrics_ = true;
        } else if (x >= 120 && x <= 420) {
            if (!lyrics_.empty()) show_lyrics_ = !show_lyrics_;
        }
        return;
    }

    if (screen_ == Screen::Login) {
        if (x >= 520 && x <= 760 && y >= 600 && y <= 648 &&
            clockSeconds() >= refresh_allowed_at_) {
            startLogin();
        }
        return;
    }

    if (x < kSidebarWidth) {
        for (int i = 0; i < 4; ++i) {
            int top = 118 + i * 58;
            if (y >= top - 10 && y <= top + 48) {
                tab_ = i;
                if (i == 2 && recent_.empty()) loadRecent();
                if (i == 3) startSearch();
                return;
            }
        }
        return;
    }

    if (x < 320 || y < kListTop) return;

    int row = (int)((y - kListTop) / kRowHeight);

    if (screen_ == Screen::Home) {
        if (tab_ == 0 && !daily_.empty()) {
            int index = daily_scroll_ + row;
            if (index >= 0 && index < (int)daily_.size()) {
                daily_sel_ = index;
                play_queue_ = daily_;
                playing_index_ = index;
                playSong(daily_[index]);
            }
        } else if (tab_ == 1 && !playlists_.empty()) {
            int index = playlist_scroll_ + row;
            if (index >= 0 && index < (int)playlists_.size()) {
                playlist_sel_ = index;
                openPlaylist(playlists_[index].id, playlists_[index].name);
            }
        } else if (tab_ == 2 && !recent_.empty()) {
            int index = recent_scroll_ + row;
            if (index >= 0 && index < (int)recent_.size()) {
                recent_sel_ = index;
                play_queue_ = recent_;
                playing_index_ = index;
                playSong(recent_[index]);
            }
        }
    } else if (screen_ == Screen::Playlist && !tracks_.empty()) {
        int index = track_scroll_ + row;
        if (index >= 0 && index < (int)tracks_.size()) {
            track_sel_ = index;
            play_queue_ = tracks_;
            playing_index_ = index;
            playSong(tracks_[index]);
        }
    }
}

void App::handleDrag(float, float dy) {
    if (screen_ == Screen::Login || show_lyrics_) return;
    if (touch_start_y_ < kListTop || touch_start_y_ > kNowBarY) return;

    drag_accum_ += dy;
    int step = 0;
    while (drag_accum_ >= 30.0f) { ++step; drag_accum_ -= 30.0f; }
    while (drag_accum_ <= -30.0f) { --step; drag_accum_ += 30.0f; }
    if (step == 0) return;

    // Finger moving down reveals earlier rows, so the scroll offset decreases.
    int delta = -step;

    if (screen_ == Screen::Home) {
        if (tab_ == 0 && !daily_.empty()) {
            int max_scroll = std::max(0, (int)daily_.size() - kVisibleRows);
            daily_scroll_ = std::clamp(daily_scroll_ + delta, 0, max_scroll);
        } else if (tab_ == 1 && !playlists_.empty()) {
            int max_scroll = std::max(0, (int)playlists_.size() - kVisibleRows);
            playlist_scroll_ = std::clamp(playlist_scroll_ + delta, 0, max_scroll);
        } else if (tab_ == 2 && !recent_.empty()) {
            int max_scroll = std::max(0, (int)recent_.size() - kVisibleRows);
            recent_scroll_ = std::clamp(recent_scroll_ + delta, 0, max_scroll);
        }
    } else if (screen_ == Screen::Playlist && !tracks_.empty()) {
        int max_scroll = std::max(0, (int)tracks_.size() - kVisibleRows);
        track_scroll_ = std::clamp(track_scroll_ + delta, 0, max_scroll);
    }
}

void App::pollInput() {
    pollTouch();
    padUpdate(&pad_);
    down_ = padGetButtonsDown(&pad_);
    held_ = padGetButtons(&pad_);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) running_ = false;
    }

    if (pressed(HidNpadButton_B)) {
        if (show_lyrics_) {
            show_lyrics_ = false;
        } else if (screen_ == Screen::Playlist) {
            screen_ = Screen::Home;
        }
    }

    if (pressed(HidNpadButton_X)) player_.togglePause();
    if (pressed(HidNpadButton_Y)) {
        if (lyrics_.empty()) {
            toast("当前歌曲暂无歌词");
        } else {
            show_lyrics_ = !show_lyrics_;
        }
    }
    if (pressed(HidNpadButton_L)) playAdjacent(-1);
    if (pressed(HidNpadButton_R)) playAdjacent(1);
    if (pressedRepeat(HidNpadButton_ZL, repeat_vol_down_next_)) player_.adjustVolume(-0.05f);
    if (pressedRepeat(HidNpadButton_ZR, repeat_vol_up_next_)) player_.adjustVolume(0.05f);

    if (screen_ == Screen::Login) {
        if (pressed(HidNpadButton_A) && clockSeconds() >= refresh_allowed_at_) startLogin();
    } else if (screen_ == Screen::Home) {
        if (pressed(HidNpadButton_Left)) {
            tab_ = (tab_ + 3) % 4;
            if (tab_ == 2 && recent_.empty()) loadRecent();
        }
        if (pressed(HidNpadButton_Right)) {
            tab_ = (tab_ + 1) % 4;
            if (tab_ == 2 && recent_.empty()) loadRecent();
        }

        if (tab_ == 0 && !daily_.empty()) {
            if (pressedRepeat(HidNpadButton_Up, repeat_up_next_)) {
                daily_sel_ = std::max(0, daily_sel_ - 1);
            }
            if (pressedRepeat(HidNpadButton_Down, repeat_down_next_)) {
                daily_sel_ = std::min((int)daily_.size() - 1, daily_sel_ + 1);
            }
            keepVisible(daily_scroll_, daily_sel_, kVisibleRows);
        } else if (tab_ == 1 && !playlists_.empty()) {
            if (pressedRepeat(HidNpadButton_Up, repeat_up_next_)) {
                playlist_sel_ = std::max(0, playlist_sel_ - 1);
            }
            if (pressedRepeat(HidNpadButton_Down, repeat_down_next_)) {
                playlist_sel_ = std::min((int)playlists_.size() - 1, playlist_sel_ + 1);
            }
            keepVisible(playlist_scroll_, playlist_sel_, kVisibleRows);
        } else if (tab_ == 2 && !recent_.empty()) {
            if (pressedRepeat(HidNpadButton_Up, repeat_up_next_)) {
                recent_sel_ = std::max(0, recent_sel_ - 1);
            }
            if (pressedRepeat(HidNpadButton_Down, repeat_down_next_)) {
                recent_sel_ = std::min((int)recent_.size() - 1, recent_sel_ + 1);
            }
            keepVisible(recent_scroll_, recent_sel_, kVisibleRows);
        }

        if (pressed(HidNpadButton_A)) openSelected();
        if (pressed(HidNpadButton_Plus)) refreshHomePage();
    } else if (screen_ == Screen::Playlist) {
        if (!tracks_.empty()) {
            if (pressedRepeat(HidNpadButton_Up, repeat_up_next_)) {
                track_sel_ = std::max(0, track_sel_ - 1);
            }
            if (pressedRepeat(HidNpadButton_Down, repeat_down_next_)) {
                track_sel_ = std::min((int)tracks_.size() - 1, track_sel_ + 1);
            }
            keepVisible(track_scroll_, track_sel_, kVisibleRows);
        }
        if (pressed(HidNpadButton_A)) playSelectedFromPlaylist();
        if (pressed(HidNpadButton_Left)) player_.seek(player_.position() - 10.0);
        if (pressed(HidNpadButton_Right)) player_.seek(player_.position() + 10.0);
    }
}

void App::update() {
    pollLogin();

    std::string error = player_.error();
    if (player_.state() == Player::State::Error && !error.empty() && error != last_player_error_) {
        last_player_error_ = error;
        toast(error);
    }

    if (player_.finished()) {
        double now = clockSeconds();
        if (now >= auto_next_at_) {
            auto_next_at_ = now + 2.0;
            if (!play_queue_.empty() && playing_index_ >= 0) playAdjacent(1);
        }
    }
}

void App::redrawNow() {
    draw();
    SDL_RenderPresent(renderer_);
}

void App::draw() {
    ui_.clear(kBg);

    switch (screen_) {
    case Screen::Login:
        drawLogin();
        break;
    case Screen::Home:
        drawHome();
        drawNowBar();
        break;
    case Screen::Playlist:
        drawPlaylist();
        drawNowBar();
        break;
    }

    if (show_lyrics_) drawLyrics();
    drawToast();
}

void App::drawSidebar() {
    ui_.rect(0, 0, kSidebarWidth, kNowBarY, kSidebarBg);
    ui_.text(32, 26, "网易云音乐", 28, kAccent);
    if (!nickname_.empty()) {
        ui_.text(32, 66, utf8Truncate(nickname_, 16), 16, kSub);
    }

    const char *items[] = {"每日推荐", "我的歌单", "最近播放", "搜索"};
    for (int i = 0; i < 4; ++i) {
        int y = 128 + i * 58;
        if (tab_ == i) {
            ui_.rect(16, y - 10, 268, 48, kRowSelected);
            ui_.rect(16, y - 10, 4, 48, kAccent);
        }
        ui_.text(40, y, items[i], 21, tab_ == i ? kText : kSub);
    }

    ui_.text(32, 520, "A 确认   B 返回", 15, kSub);
    ui_.text(32, 544, "X 暂停   Y 歌词", 15, kSub);
    ui_.text(32, 568, "L/R 切歌  ZL/ZR 音量", 15, kSub);
}

void App::drawTrackRow(const Song &song, int index, int y, bool selected) {
    if (selected) {
        ui_.rect(320, y, 940, 52, kRowSelected);
        ui_.rect(320, y, 4, 52, kAccent);
    }

    char number[16];
    snprintf(number, sizeof(number), "%d", index + 1);
    ui_.text(344, y + 8, number, 16, kSub);

    ui_.text(392, y + 4, utf8Truncate(song.name, 34), 19, kText);
    ui_.text(392, y + 28, utf8Truncate(song.artist, 30), 15, kSub);

    ui_.textRight(1240, y + 14, formatDuration(song.duration_ms), 16, kSub);
}

void App::drawScrollbar(int x, int y, int height, int visible, int total, int first) {
    if (total <= visible) return;
    int bar_height = std::max(24, height * visible / total);
    int max_first = std::max(1, total - visible);
    int offset = (height - bar_height) * first / max_first;
    ui_.rect(x, y + offset, 4, bar_height, rgba(90, 90, 100));
}

void App::drawHome() {
    ui_.rect(kSidebarWidth, 0, 1280 - kSidebarWidth, kNowBarY, kBg);
    drawSidebar();

    const char *title = tab_ == 0 ? "每日推荐"
                        : tab_ == 1 ? "我的歌单"
                        : tab_ == 2 ? "最近播放"
                                    : "搜索";
    ui_.text(340, 30, title, 26, kText);
    ui_.text(340, 66, tab_ == 2 ? "按 A 打开键盘输入关键词" : "按 + 刷新", 15, kSub);

    if (tab_ == 0) {
        if (daily_.empty()) {
            ui_.text(340, kListTop + 20, "暂无数据，按 + 刷新", 18, kSub);
            return;
        }
        for (int i = 0; i < kVisibleRows; ++i) {
            int index = daily_scroll_ + i;
            if (index >= (int)daily_.size()) break;
            drawTrackRow(daily_[index], index, kListTop + i * kRowHeight,
                         index == daily_sel_);
        }
        drawScrollbar(1264, kListTop, kVisibleRows * kRowHeight, kVisibleRows,
                      (int)daily_.size(), daily_scroll_);
    } else if (tab_ == 1) {
        if (playlists_.empty()) {
            ui_.text(340, kListTop + 20, "暂无歌单，按 + 刷新", 18, kSub);
            return;
        }
        for (int i = 0; i < kVisibleRows; ++i) {
            int index = playlist_scroll_ + i;
            if (index >= (int)playlists_.size()) break;
            const Playlist &p = playlists_[index];
            int y = kListTop + i * kRowHeight;
            bool selected = index == playlist_sel_;
            if (selected) {
                ui_.rect(320, y, 940, 52, kRowSelected);
                ui_.rect(320, y, 4, 52, kAccent);
            }
            ui_.text(344, y + 12, utf8Truncate(p.name, 32), 19, kText);
            char count[48];
            snprintf(count, sizeof(count), "%d 首", p.track_count);
            ui_.textRight(1240, y + 14, count, 16, kSub);
        }
        drawScrollbar(1264, kListTop, kVisibleRows * kRowHeight, kVisibleRows,
                      (int)playlists_.size(), playlist_scroll_);
    } else if (tab_ == 2) {
        if (recent_.empty()) {
            ui_.text(340, kListTop + 20, "暂无最近播放，按 + 刷新", 18, kSub);
            return;
        }
        for (int i = 0; i < kVisibleRows; ++i) {
            int index = recent_scroll_ + i;
            if (index >= (int)recent_.size()) break;
            drawTrackRow(recent_[index], index, kListTop + i * kRowHeight,
                         index == recent_sel_);
        }
        drawScrollbar(1264, kListTop, kVisibleRows * kRowHeight, kVisibleRows,
                      (int)recent_.size(), recent_scroll_);
    } else {
        ui_.text(340, kListTop + 20, "搜索歌曲、歌手或专辑", 18, kSub);
        ui_.text(340, kListTop + 52, "按 A 调出系统键盘", 16, kSub);
    }
}

void App::drawPlaylist() {
    ui_.rect(kSidebarWidth, 0, 1280 - kSidebarWidth, kNowBarY, kBg);
    drawSidebar();

    ui_.text(340, 30, utf8Truncate(tracks_title_, 28), 26, kText);
    char count[64];
    snprintf(count, sizeof(count), "%d 首    B 返回", (int)tracks_.size());
    ui_.text(340, 66, count, 15, kSub);

    if (tracks_.empty()) {
        ui_.text(340, kListTop + 20, "暂无歌曲", 18, kSub);
        return;
    }

    for (int i = 0; i < kVisibleRows; ++i) {
        int index = track_scroll_ + i;
        if (index >= (int)tracks_.size()) break;
        drawTrackRow(tracks_[index], index, kListTop + i * kRowHeight, index == track_sel_);
    }

    drawScrollbar(1264, kListTop, kVisibleRows * kRowHeight, kVisibleRows, (int)tracks_.size(),
                  track_scroll_);
}

void App::drawNowBar() {
    ui_.rect(0, kNowBarY, 1280, 720 - kNowBarY, kPanelBg);
    ui_.rect(0, kNowBarY, 1280, 1, rgba(50, 50, 58));

    if (player_.state() == Player::State::Idle) {
        ui_.text(32, kNowBarY + 30, "未在播放", 18, kSub);
        return;
    }

    bool paused = player_.state() == Player::State::Paused;
    bool loading = player_.state() == Player::State::Loading;
    ui_.rect(24, 644, 88, 52, paused || loading ? rgba(60, 60, 68) : kAccent);
    ui_.textCentered(68, 658, paused ? "播放" : "暂停", 19, kText);

    ui_.text(128, kNowBarY + 12, utf8Truncate(player_.title(), 24), 20, kText);
    ui_.text(128, kNowBarY + 42, utf8Truncate(player_.artist(), 28), 15, kSub);

    const char *state_text = "";
    switch (player_.state()) {
    case Player::State::Loading: state_text = "缓冲中…"; break;
    case Player::State::Playing: state_text = "正在播放"; break;
    case Player::State::Paused: state_text = "已暂停"; break;
    case Player::State::Error: state_text = "播放失败"; break;
    case Player::State::Finished: state_text = "播放结束"; break;
    default: break;
    }
    ui_.text(128, kNowBarY + 62, state_text, 14, kSub);

    double position = player_.position();
    double duration = player_.duration();
    double ratio = duration > 0 ? position / duration : 0.0;
    ratio = std::clamp(ratio, 0.0, 1.0);

    int bar_x = 420;
    int bar_w = 680;
    int bar_y = kNowBarY + 40;
    ui_.rect(bar_x, bar_y, bar_w, 6, rgba(60, 60, 68));
    ui_.rect(bar_x, bar_y, (int)(bar_w * ratio), 6, kAccent);

    ui_.text(bar_x, bar_y + 12, formatTime(position), 14, kSub);
    ui_.textRight(bar_x + bar_w, bar_y + 12, formatTime(duration), 14, kSub);

    ui_.rect(1120, 644, 120, 52, rgba(52, 52, 62));
    ui_.textCentered(1180, 658, "歌词", 19, kText);
    ui_.textCentered(1180, 682, "Y", 12, kSub);
}

void App::drawLyrics() {
    ui_.rect(kSidebarWidth, 0, 1280 - kSidebarWidth, kNowBarY, rgba(12, 12, 16, 240));
    ui_.text(kSidebarWidth + 40, 24, "歌词", 24, kText);

    if (lyrics_.empty()) {
        ui_.textCentered(kSidebarWidth + (1280 - kSidebarWidth) / 2, 300, "暂无歌词", 22, kSub);
        return;
    }

    double position = player_.position();
    int current = 0;
    for (int i = 0; i < (int)lyrics_.size(); ++i) {
        if (lyrics_[i].time <= position + 0.2) current = i;
        else break;
    }

    int cx = kSidebarWidth + 40;
    int width = 1280 - kSidebarWidth - 120;
    int center_y = 320;
    int line_height = 44;

    for (int offset = -3; offset <= 4; ++offset) {
        int index = current + offset;
        if (index < 0 || index >= (int)lyrics_.size()) continue;
        int y = center_y + offset * line_height;
        uint32_t color = offset == 0 ? kText : rgba(120, 120, 132);
        int size = offset == 0 ? 24 : 18;
        ui_.textWrapped(cx, y, width, lyrics_[index].text, size, color, line_height);
    }
}

void App::drawLogin() {
    ui_.clear(kBg);
    ui_.textCentered(640, 74, "网易云音乐", 44, kAccent);
    ui_.textCentered(640, 136, "Nintendo Switch 客户端", 18, kSub);

    int card_x = 460;
    int card_y = 190;
    int card_size = 340;
    ui_.rect(card_x, card_y, card_size, card_size, kWhite);

    if (!qr_url_.empty()) {
        using qrcodegen::QrCode;
        QrCode qr = QrCode::encodeText(qr_url_.c_str(), QrCode::Ecc::LOW);
        int modules = qr.getSize();
        int scale = std::max(1, 300 / modules);
        int total = modules * scale;
        int ox = card_x + (card_size - total) / 2;
        int oy = card_y + (card_size - total) / 2;
        for (int y = 0; y < modules; ++y) {
            for (int x = 0; x < modules; ++x) {
                if (qr.getModule(x, y)) {
                    ui_.rect(ox + x * scale, oy + y * scale, scale, scale, rgba(0, 0, 0));
                }
            }
        }
    }

    std::string status;
    switch (qr_code_) {
    case 800: status = "二维码已过期，按 A 刷新"; break;
    case 801: status = "请使用网易云音乐 App 扫描二维码"; break;
    case 802: status = "已扫描，请在手机上确认登录"; break;
    case 803: status = "登录成功"; break;
    case -1: status = "网络异常，2 秒后重试"; break;
    default:
        status = qr_code_ > 803 ? ("登录失败（" + std::to_string(qr_code_) + "），按 A 刷新")
                                : "正在获取二维码…";
        break;
    }
    ui_.textCentered(640, 556, status, 20, kText);

    if (!net_error_.empty()) {
        ui_.textCentered(640, 574, net_error_, 15, kAccent);
    } else if (!qr_key_.empty()) {
        ui_.textCentered(640, 574, "二维码有效期约 5 分钟", 14, kSub);
    }

    double remain = refresh_allowed_at_ - clockSeconds();
    bool ready = remain <= 0.0;
    ui_.rect(520, 600, 240, 48, ready ? kAccent : rgba(60, 60, 68));
    std::string label = ready ? "刷新二维码"
                              : ("刷新（" + std::to_string((int)std::ceil(remain)) + " 秒）");
    ui_.textCentered(640, 613, label, 18, kText);
}

void App::drawToast() {
    if (toast_text_.empty()) return;
    if (clockSeconds() > toast_until_) {
        toast_text_.clear();
        return;
    }
    int width = ui_.textWidth(toast_text_, 17);
    int box_w = std::min(1100, width + 48);
    int x = (1280 - box_w) / 2;
    int y = show_lyrics_ ? 596 : 596;
    ui_.rect(x, y, box_w, 44, rgba(40, 40, 48, 235));
    ui_.textCentered(640, y + 11, toast_text_, 17, kText);
}

} // namespace ntv
