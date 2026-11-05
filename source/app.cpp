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

const int kBrValues[] = {128000, 192000, 320000, 999000};
const char *kBrNames[] = {"128K", "192K", "320K", "无损"};

constexpr int kSidebarWidth = 300;
constexpr int kNowBarY = 632;
constexpr int kRowHeight = 56;
constexpr int kVisibleRows = 9;
constexpr int kListTop = 88;

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

    http_.loadCookieText("sdmc:/switch/netease-switch/cookie.txt");
    {
        std::string manual = http_.cookie("MUSIC_U");
        if (!manual.empty() && manual != api_.musicU()) {
            logf("[login] using MUSIC_U from cookie.txt");
            api_.setMusicU(manual);
        }
    }

    FILE *quality_file = fopen("sdmc:/switch/netease-switch/quality.txt", "r");
    if (quality_file) {
        int value = 0;
        if (fscanf(quality_file, "%d", &value) == 1 && value >= 0 && value < 4) {
            quality_index_ = value;
        }
        fclose(quality_file);
    }
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
        std::string cookie = http_.cookie("MUSIC_U");
        if (cookie.empty()) cookie = api_.musicU();
        if (!cookie.empty()) api_.setMusicU(cookie);
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
    ok = api_.userPlaylists(playlists_) || ok;
    liked_playlist_id_ = 0;
    liked_track_count_ = 0;
    for (const auto &p : playlists_) {
        if (p.special_type == 5) {
            liked_playlist_id_ = p.id;
            liked_track_count_ = p.track_count;
            break;
        }
    }
    if (!ok) toast("加载失败，按 + 重试");
}

void App::loadRecent() {
    redrawNow();
    std::vector<Song> songs;
    if (!api_.recentSongs(songs)) {
        toast("最近播放加载失败");
        return;
    }
    recent_ = songs;
    recent_sel_ = 0;
    recent_scroll_ = 0;
}

void App::loadToplists() {
    redrawNow();
    std::vector<Playlist> lists;
    if (!api_.toplists(lists)) {
        toast("排行榜加载失败");
        return;
    }
    toplists_ = lists;
    top_sel_ = 0;
    top_scroll_ = 0;
}

void App::loadRecommend() {
    redrawNow();
    std::vector<Playlist> lists;
    if (!api_.recommendPlaylists(lists)) {
        toast("推荐歌单加载失败");
        return;
    }
    recommend_ = lists;
    rec_sel_ = 0;
    rec_scroll_ = 0;
}

void App::ensureLikedIds() {
    if (liked_ids_loaded_ || liked_playlist_id_ == 0) return;
    redrawNow();
    std::vector<Song> songs;
    std::string name;
    if (!api_.playlistDetail(liked_playlist_id_, songs, name)) return;
    liked_ids_.clear();
    for (const auto &song : songs) liked_ids_.insert(song.id);
    liked_ids_loaded_ = true;
}

void App::toggleLikeCurrent() {
    if (current_song_id_ == 0) {
        toast("当前没有播放歌曲");
        return;
    }
    if (liked_playlist_id_ == 0) {
        toast("未找到我喜欢的音乐");
        return;
    }

    redrawNow();
    ensureLikedIds();

    bool liked = liked_ids_.count(current_song_id_) != 0;
    bool target = !liked;
    if (!api_.likeSong(current_song_id_, target)) {
        toast("操作失败，请稍后重试");
        return;
    }

    if (target) {
        liked_ids_.insert(current_song_id_);
        if (liked_track_count_ >= 0) ++liked_track_count_;
    } else {
        liked_ids_.erase(current_song_id_);
        if (liked_track_count_ > 0) --liked_track_count_;
    }
    toast(target ? "已加入我喜欢的音乐" : "已取消喜欢");
}

void App::toggleSidebar() {
    sidebar_collapsed_ = !sidebar_collapsed_;
}

int App::sidebarWidth() const {
    return sidebar_collapsed_ ? 88 : 300;
}

int App::contentX() const {
    return sidebarWidth() + 20;
}

int App::contentWidth() const {
    return 1280 - contentX() - 20;
}

void App::loadCover(const Song &song) {
    cover_ = nullptr;
    cover_key_ = song.cover_url;
    if (cover_key_.empty()) return;

    if (SDL_Texture *cached = ui_.cachedImage(cover_key_)) {
        cover_ = cached;
        return;
    }

    std::vector<unsigned char> bytes;
    if (!http_.download(cover_key_, bytes)) return;
    cover_ = ui_.textureFromMemory(cover_key_, bytes);
}

void App::cycleQuality() {
    quality_index_ = (quality_index_ + 1) % 4;
    FILE *f = fopen("sdmc:/switch/netease-switch/quality.txt", "w");
    if (f) {
        fprintf(f, "%d\n", quality_index_);
        fclose(f);
    }
    toast(std::string("音质：") + kBrNames[quality_index_]);
}

void App::ensureTabLoaded() {
    if (tab_ == 3 && recent_.empty()) loadRecent();
    else if (tab_ == 4 && toplists_.empty()) loadToplists();
    else if (tab_ == 5 && recommend_.empty()) loadRecommend();
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
    } else if (tab_ == 1 || tab_ == 2) {
        std::vector<Playlist> lists;
        if (api_.userPlaylists(lists)) {
            playlists_ = lists;
            playlist_sel_ = std::min(playlist_sel_, (int)playlists_.size() - 1);
            if (playlist_sel_ < 0) playlist_sel_ = 0;
            liked_playlist_id_ = 0;
            liked_track_count_ = 0;
            for (const auto &p : playlists_) {
                if (p.special_type == 5) {
                    liked_playlist_id_ = p.id;
                    liked_track_count_ = p.track_count;
                    break;
                }
            }
            toast("已刷新");
        } else {
            toast("刷新失败");
        }
    } else if (tab_ == 3) {
        loadRecent();
    } else if (tab_ == 4) {
        loadToplists();
    } else if (tab_ == 5) {
        loadRecommend();
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
        if (liked_playlist_id_ == 0) {
            toast("未找到我喜欢的音乐");
            return;
        }
        openPlaylist(liked_playlist_id_, "我喜欢的音乐");
    } else if (tab_ == 3) {
        if (recent_.empty()) {
            loadRecent();
            return;
        }
        play_queue_ = recent_;
        playing_index_ = recent_sel_;
        playSong(recent_[recent_sel_]);
    } else if (tab_ == 4) {
        if (toplists_.empty()) {
            loadToplists();
            return;
        }
        const Playlist &p = toplists_[top_sel_];
        openPlaylist(p.id, p.name);
    } else if (tab_ == 5) {
        if (recommend_.empty()) {
            loadRecommend();
            return;
        }
        const Playlist &p = recommend_[rec_sel_];
        openPlaylist(p.id, p.name);
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
    if (id == liked_playlist_id_) {
        liked_ids_.clear();
        for (const auto &song : tracks_) liked_ids_.insert(song.id);
        liked_ids_loaded_ = true;
    }
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
    if (!api_.songUrl(song.id, kBrValues[quality_index_], url, error)) {
        toast(error);
        auto_next_at_ = clockSeconds() + 60.0;
        return;
    }

    player_.play(url, song.name, song.artist);
    current_song_id_ = song.id;
    loadCover(song);
    loadLyrics(song.id);
    show_lyrics_ = false;
    last_player_error_.clear();
    auto_next_at_ = clockSeconds() + 2.0;
}

void App::playAdjacent(int direction) {
    if (play_queue_.empty()) return;

    int count = (int)play_queue_.size();
    if (playing_index_ < 0) playing_index_ = 0;

    if (play_mode_ == PlayMode::RepeatOne && direction > 0) {
        playSong(play_queue_[playing_index_]);
        return;
    }

    if (play_mode_ == PlayMode::Shuffle && count > 1 && direction > 0) {
        int next = playing_index_;
        while (next == playing_index_) next = (int)(randomGet64() % (uint64_t)count);
        playing_index_ = next;
    } else {
        playing_index_ = (playing_index_ + direction + count) % count;
    }
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
    hidGetTouchScreenStates(&state, 1);
    bool down = state.count > 0;

    if (down) {
        const HidTouchState &touch = state.touches[0];
        float x = (float)touch.x;
        float y = (float)touch.y;
        if (!touch_logged_) {
            logf("[touch] first contact x=%.0f y=%.0f fingers=%d", x, y, (int)state.count);
            touch_logged_ = true;
        }

        if (!touch_active_) {
            touch_active_ = true;
            touch_dragged_ = false;
            touch_move_ = 0;
            touch_last_x_ = x;
            touch_last_y_ = y;
            touch_start_x_ = x;
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
        } else if (x >= 420 && x <= 940 && y >= 660 && y <= 700) {
            double total = player_.duration();
            if (total > 0) player_.seek(((x - 420.0) / 520.0) * total);
        } else if (x >= 956 && x <= 1026 && y >= 640) {
            toggleLikeCurrent();
        } else if (x >= 1032 && x <= 1102 && y >= 640) {
            cycleQuality();
        } else if (x >= 1108 && y >= 640) {
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

    int w = sidebarWidth();
    if (x < w) {
        int bx = sidebar_collapsed_ ? w - 40 : w - 44;
        if (y >= 12 && y <= 46 && x >= bx - 10 && x <= bx + 34) {
            toggleSidebar();
            return;
        }

        for (int i = 0; i < 7; ++i) {
            int top = 112 + i * 54;
            if (y >= top && y <= top + 44) {
                tab_ = i;
                sidebar_focus_ = true;
                ensureTabLoaded();
                if (i == 6) startSearch();
                return;
            }
        }
        return;
    }

    if (x >= 1280 - 140 && y >= 20 && y <= 62) {
        refreshHomePage();
        return;
    }

    if (x < contentX() || y < kListTop) return;

    sidebar_focus_ = false;
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
        } else if (tab_ == 2) {
            if (row == 0 && liked_playlist_id_ != 0) {
                openPlaylist(liked_playlist_id_, "我喜欢的音乐");
            }
        } else if (tab_ == 3 && !recent_.empty()) {
            int index = recent_scroll_ + row;
            if (index >= 0 && index < (int)recent_.size()) {
                recent_sel_ = index;
                play_queue_ = recent_;
                playing_index_ = index;
                playSong(recent_[index]);
            }
        } else if (tab_ == 4 && !toplists_.empty()) {
            int index = top_scroll_ + row;
            if (index >= 0 && index < (int)toplists_.size()) {
                top_sel_ = index;
                openPlaylist(toplists_[index].id, toplists_[index].name);
            }
        } else if (tab_ == 5 && !recommend_.empty()) {
            int index = rec_scroll_ + row;
            if (index >= 0 && index < (int)recommend_.size()) {
                rec_sel_ = index;
                openPlaylist(recommend_[index].id, recommend_[index].name);
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
    if (touch_start_x_ < sidebarWidth()) return;
    if (touch_start_y_ < kListTop || touch_start_y_ > kNowBarY) return;

    drag_accum_ += dy;
    int step = 0;
    while (drag_accum_ >= 30.0f) { ++step; drag_accum_ -= 30.0f; }
    while (drag_accum_ <= -30.0f) { --step; drag_accum_ += 30.0f; }
    if (step == 0) return;

    int delta = -step;

    if (screen_ == Screen::Home) {
        if (tab_ == 0 && !daily_.empty()) {
            int max_scroll = std::max(0, (int)daily_.size() - kVisibleRows);
            daily_scroll_ = std::clamp(daily_scroll_ + delta, 0, max_scroll);
        } else if (tab_ == 1 && !playlists_.empty()) {
            int max_scroll = std::max(0, (int)playlists_.size() - kVisibleRows);
            playlist_scroll_ = std::clamp(playlist_scroll_ + delta, 0, max_scroll);
        } else if (tab_ == 3 && !recent_.empty()) {
            int max_scroll = std::max(0, (int)recent_.size() - kVisibleRows);
            recent_scroll_ = std::clamp(recent_scroll_ + delta, 0, max_scroll);
        } else if (tab_ == 4 && !toplists_.empty()) {
            int max_scroll = std::max(0, (int)toplists_.size() - kVisibleRows);
            top_scroll_ = std::clamp(top_scroll_ + delta, 0, max_scroll);
        } else if (tab_ == 5 && !recommend_.empty()) {
            int max_scroll = std::max(0, (int)recommend_.size() - kVisibleRows);
            rec_scroll_ = std::clamp(rec_scroll_ + delta, 0, max_scroll);
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
    if (pressed(HidNpadButton_StickR)) toggleLikeCurrent();
    if (pressed(HidNpadButton_StickL)) cycleQuality();
    if (pressed(HidNpadButton_Minus)) {
        play_mode_ = play_mode_ == PlayMode::Sequential ? PlayMode::RepeatOne
                     : play_mode_ == PlayMode::RepeatOne ? PlayMode::Shuffle
                                                         : PlayMode::Sequential;
        toast(play_mode_ == PlayMode::RepeatOne ? "单曲循环"
              : play_mode_ == PlayMode::Shuffle   ? "随机播放"
                                                  : "顺序播放");
    }
    if (pressed(HidNpadButton_Plus)) toggleSidebar();

    if (screen_ == Screen::Login) {
        if (pressed(HidNpadButton_A) && clockSeconds() >= refresh_allowed_at_) startLogin();
    } else if (screen_ == Screen::Home) {
        if (pressed(HidNpadButton_Left)) sidebar_focus_ = true;
        if (pressed(HidNpadButton_Right) && sidebar_focus_) sidebar_focus_ = false;

        if (sidebar_focus_) {
            if (pressedRepeat(HidNpadButton_Up, repeat_up_next_)) {
                tab_ = (tab_ + 6) % 7;
                ensureTabLoaded();
            }
            if (pressedRepeat(HidNpadButton_Down, repeat_down_next_)) {
                tab_ = (tab_ + 1) % 7;
                ensureTabLoaded();
            }
            if (pressed(HidNpadButton_A)) {
                if (tab_ == 6) startSearch();
                else sidebar_focus_ = false;
            }
        } else {
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
            } else if (tab_ == 3 && !recent_.empty()) {
                if (pressedRepeat(HidNpadButton_Up, repeat_up_next_)) {
                    recent_sel_ = std::max(0, recent_sel_ - 1);
                }
                if (pressedRepeat(HidNpadButton_Down, repeat_down_next_)) {
                    recent_sel_ = std::min((int)recent_.size() - 1, recent_sel_ + 1);
                }
                keepVisible(recent_scroll_, recent_sel_, kVisibleRows);
            } else if (tab_ == 4 && !toplists_.empty()) {
                if (pressedRepeat(HidNpadButton_Up, repeat_up_next_)) {
                    top_sel_ = std::max(0, top_sel_ - 1);
                }
                if (pressedRepeat(HidNpadButton_Down, repeat_down_next_)) {
                    top_sel_ = std::min((int)toplists_.size() - 1, top_sel_ + 1);
                }
                keepVisible(top_scroll_, top_sel_, kVisibleRows);
            } else if (tab_ == 5 && !recommend_.empty()) {
                if (pressedRepeat(HidNpadButton_Up, repeat_up_next_)) {
                    rec_sel_ = std::max(0, rec_sel_ - 1);
                }
                if (pressedRepeat(HidNpadButton_Down, repeat_down_next_)) {
                    rec_sel_ = std::min((int)recommend_.size() - 1, rec_sel_ + 1);
                }
                keepVisible(rec_scroll_, rec_sel_, kVisibleRows);
            }

            if (pressed(HidNpadButton_A)) openSelected();
        }
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
    int w = sidebarWidth();
    ui_.rect(0, 0, w, kNowBarY, kSidebarBg);

    if (!sidebar_collapsed_) {
        ui_.text(26, 22, "网易云音乐", 25, kAccent);
        if (!nickname_.empty()) {
            ui_.text(26, 56, utf8Truncate(nickname_, 14), 14, kSub);
        }
    } else {
        ui_.rect(24, 18, 40, 40, kAccent);
        ui_.textCentered(44, 25, "云", 22, kText);
    }

    // collapse control
    ui_.text(sidebar_collapsed_ ? w - 34 : w - 40, 24, sidebar_collapsed_ ? ">" : "<", 20, kSub);

    const char *icons[] = {"日", "单", "心", "近", "榜", "荐", "搜"};
    const char *labels[] = {"每日推荐", "我的歌单", "我喜欢的", "最近播放",
                            "排行榜", "推荐歌单", "搜索"};

    for (int i = 0; i < 7; ++i) {
        int y = 112 + i * 54;
        bool selected = (tab_ == i);
        if (selected) {
            ui_.rect(8, y, w - 16, 44, kRowSelected);
            ui_.rect(8, y, 4, 44, kAccent);
        }

        int badge_x = sidebar_collapsed_ ? (w - 36) / 2 : 18;
        uint32_t badge_color = selected ? kAccent : rgba(46, 46, 56);
        ui_.rect(badge_x, y + 6, 32, 32, badge_color);
        ui_.textCentered(badge_x + 16, y + 11, icons[i], 18, selected ? kText : kSub);

        if (!sidebar_collapsed_) {
            ui_.text(62, y + 9, labels[i], 20, selected ? kText : kSub);
        }
    }

    if (!sidebar_collapsed_) {
        ui_.text(26, 498, "A 确认   B 返回", 14, kSub);
        ui_.text(26, 520, "X 暂停   Y 歌词", 14, kSub);
        ui_.text(26, 542, "L/R 切歌  ZL/ZR 音量", 14, kSub);
        ui_.text(26, 564, "R3 红心  L3 音质  - 模式", 14, kSub);
        ui_.text(26, 586, "+ 收起侧栏  左右切焦点", 14, kSub);
    }

    if (sidebar_focus_) {
        ui_.rectOutline(0, 0, w, kNowBarY, kAccent, 2);
    }
}

void App::drawTrackRow(const Song &song, int index, int y, bool selected) {
    int x = contentX();
    int w = contentWidth();

    if (selected) {
        ui_.rect(x, y, w, 52, kRowSelected);
        ui_.rect(x, y, 4, 52, kAccent);
    }

    char number[16];
    snprintf(number, sizeof(number), "%d", index + 1);
    ui_.text(x + 20, y + 8, number, 16, kSub);

    int text_width = w - 160;
    ui_.text(x + 64, y + 4, utf8Truncate(song.name, (size_t)(text_width / 19)), 19, kText);
    ui_.text(x + 64, y + 28, utf8Truncate(song.artist, (size_t)(text_width / 15)), 15, kSub);

    ui_.textRight(x + w - 20, y + 14, formatDuration(song.duration_ms), 16, kSub);
}

void App::drawScrollbar(int x, int y, int height, int visible, int total, int first) {
    if (total <= visible) return;
    int bar_height = std::max(24, height * visible / total);
    int max_first = std::max(1, total - visible);
    int offset = (height - bar_height) * first / max_first;
    ui_.rect(x, y + offset, 4, bar_height, rgba(90, 90, 100));
}

void App::drawHome() {
    int left = sidebarWidth();
    ui_.rect(left, 0, 1280 - left, kNowBarY, kBg);
    drawSidebar();

    const char *title = tab_ == 0 ? "每日推荐"
                        : tab_ == 1 ? "我的歌单"
                        : tab_ == 2 ? "我喜欢的"
                        : tab_ == 3 ? "最近播放"
                        : tab_ == 4 ? "排行榜"
                        : tab_ == 5 ? "推荐歌单"
                                    : "搜索";
    ui_.text(contentX(), 28, title, 26, kText);

    ui_.rect(1280 - 132, 24, 96, 34, rgba(52, 52, 62));
    ui_.textCentered(1280 - 84, 33, "刷新", 15, kSub);

    ui_.text(contentX(), 66,
             tab_ == 6 ? "按 A 打开键盘输入关键词" : "左右切换焦点，上下选择", 14, kSub);

    int x = contentX();
    int w = contentWidth();

    if (tab_ == 0) {
        if (daily_.empty()) {
            ui_.text(x, kListTop + 20, "暂无数据，按刷新重试", 18, kSub);
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
            ui_.text(x, kListTop + 20, "暂无歌单，按刷新重试", 18, kSub);
            return;
        }
        for (int i = 0; i < kVisibleRows; ++i) {
            int index = playlist_scroll_ + i;
            if (index >= (int)playlists_.size()) break;
            const Playlist &p = playlists_[index];
            int y = kListTop + i * kRowHeight;
            bool selected = index == playlist_sel_;
            if (selected) {
                ui_.rect(x, y, w, 52, kRowSelected);
                ui_.rect(x, y, 4, 52, kAccent);
            }
            ui_.text(x + 24, y + 9, utf8Truncate(p.name, 30), 19, kText);
            char count[48];
            snprintf(count, sizeof(count), "%d 首", p.track_count);
            ui_.textRight(x + w - 20, y + 14, count, 16, kSub);
        }
        drawScrollbar(1264, kListTop, kVisibleRows * kRowHeight, kVisibleRows,
                      (int)playlists_.size(), playlist_scroll_);
    } else if (tab_ == 2) {
        if (liked_playlist_id_ == 0) {
            ui_.text(x, kListTop + 20, "未找到我喜欢的音乐，按刷新重试", 18, kSub);
            return;
        }
        int y = kListTop;
        ui_.rect(x, y, w, 52, kRowSelected);
        ui_.rect(x, y, 4, 52, kAccent);
        ui_.text(x + 24, y + 12, "我喜欢的音乐", 20, kText);
        char count[48];
        snprintf(count, sizeof(count), "%d 首", liked_track_count_);
        ui_.textRight(x + w - 20, y + 14, count, 16, kSub);
    } else if (tab_ == 3) {
        if (recent_.empty()) {
            ui_.text(x, kListTop + 20, "暂无最近播放，按刷新重试", 18, kSub);
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
    } else if (tab_ == 4) {
        if (toplists_.empty()) {
            ui_.text(x, kListTop + 20, "暂无榜单，按刷新重试", 18, kSub);
            return;
        }
        for (int i = 0; i < kVisibleRows; ++i) {
            int index = top_scroll_ + i;
            if (index >= (int)toplists_.size()) break;
            const Playlist &p = toplists_[index];
            int y = kListTop + i * kRowHeight;
            bool selected = index == top_sel_;
            if (selected) {
                ui_.rect(x, y, w, 52, kRowSelected);
                ui_.rect(x, y, 4, 52, kAccent);
            }
            ui_.text(x + 24, y + 7, utf8Truncate(p.name, 30), 19, kText);
            if (!p.creator.empty()) ui_.text(x + 24, y + 29, utf8Truncate(p.creator, 36), 14, kSub);
        }
        drawScrollbar(1264, kListTop, kVisibleRows * kRowHeight, kVisibleRows,
                      (int)toplists_.size(), top_scroll_);
    } else if (tab_ == 5) {
        if (recommend_.empty()) {
            ui_.text(x, kListTop + 20, "暂无推荐歌单，按刷新重试", 18, kSub);
            return;
        }
        for (int i = 0; i < kVisibleRows; ++i) {
            int index = rec_scroll_ + i;
            if (index >= (int)recommend_.size()) break;
            const Playlist &p = recommend_[index];
            int y = kListTop + i * kRowHeight;
            bool selected = index == rec_sel_;
            if (selected) {
                ui_.rect(x, y, w, 52, kRowSelected);
                ui_.rect(x, y, 4, 52, kAccent);
            }
            ui_.text(x + 24, y + 7, utf8Truncate(p.name, 30), 19, kText);
            if (!p.creator.empty()) ui_.text(x + 24, y + 29, utf8Truncate(p.creator, 36), 14, kSub);
        }
        drawScrollbar(1264, kListTop, kVisibleRows * kRowHeight, kVisibleRows,
                      (int)recommend_.size(), rec_scroll_);
    } else {
        ui_.text(x, kListTop + 20, "搜索歌曲、歌手或专辑", 18, kSub);
        ui_.text(x, kListTop + 52, "按 A 调出系统键盘", 16, kSub);
    }
}

void App::drawPlaylist() {
    int left = sidebarWidth();
    ui_.rect(left, 0, 1280 - left, kNowBarY, kBg);
    drawSidebar();

    ui_.text(contentX(), 28, utf8Truncate(tracks_title_, 26), 26, kText);
    char count[64];
    snprintf(count, sizeof(count), "%d 首    B 返回", (int)tracks_.size());
    ui_.text(contentX(), 66, count, 14, kSub);

    if (tracks_.empty()) {
        ui_.text(contentX(), kListTop + 20, "暂无歌曲", 18, kSub);
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

    if (cover_) ui_.image(cover_, 128, 648, 52, 52);
    int text_x = cover_ ? 192 : 128;
    ui_.text(text_x, kNowBarY + 12, utf8Truncate(player_.title(), cover_ ? 18 : 24), 20, kText);
    ui_.text(text_x, kNowBarY + 42, utf8Truncate(player_.artist(), cover_ ? 22 : 28), 15, kSub);

    const char *state_text = "";
    switch (player_.state()) {
    case Player::State::Loading: state_text = "缓冲中…"; break;
    case Player::State::Playing: state_text = "正在播放"; break;
    case Player::State::Paused: state_text = "已暂停"; break;
    case Player::State::Error: state_text = "播放失败"; break;
    case Player::State::Finished: state_text = "播放结束"; break;
    default: break;
    }
    const char *mode_text = play_mode_ == PlayMode::RepeatOne ? "单曲循环"
                            : play_mode_ == PlayMode::Shuffle   ? "随机播放"
                                                                : "顺序播放";
    ui_.text(text_x, kNowBarY + 62, std::string(state_text) + " · " + mode_text, 14, kSub);

    double position = player_.position();
    double duration = player_.duration();
    double ratio = duration > 0 ? position / duration : 0.0;
    ratio = std::clamp(ratio, 0.0, 1.0);

    int bar_x = 420;
    int bar_w = 520;
    int bar_y = kNowBarY + 40;
    ui_.rect(bar_x, bar_y, bar_w, 6, rgba(60, 60, 68));
    ui_.rect(bar_x, bar_y, (int)(bar_w * ratio), 6, kAccent);

    ui_.text(bar_x, bar_y + 12, formatTime(position), 14, kSub);
    ui_.textRight(bar_x + bar_w, bar_y + 12, formatTime(duration), 14, kSub);

    bool liked = current_song_id_ != 0 && liked_ids_.count(current_song_id_) != 0;
    ui_.rect(956, 644, 70, 52, liked ? kAccent : rgba(52, 52, 62));
    ui_.textCentered(991, 658, liked ? "已喜欢" : "喜欢", 14, kText);

    ui_.rect(1032, 644, 70, 52, rgba(52, 52, 62));
    ui_.textCentered(1067, 660, kBrNames[quality_index_], 16, kText);

    ui_.rect(1108, 644, 132, 52, rgba(52, 52, 62));
    ui_.textCentered(1174, 658, "歌词", 19, kText);
    ui_.textCentered(1174, 682, "Y", 12, kSub);
}

void App::drawLyrics() {
    int left = sidebarWidth();
    ui_.rect(left, 0, 1280 - left, kNowBarY, rgba(12, 12, 16, 242));
    ui_.text(left + 24, 22, "歌词", 24, kText);

    if (lyrics_.empty()) {
        ui_.textCentered(left + (1280 - left) / 2, 300, "暂无歌词", 22, kSub);
        return;
    }

    double position = player_.position();
    int current = 0;
    for (int i = 0; i < (int)lyrics_.size(); ++i) {
        if (lyrics_[i].time <= position + 0.2) current = i;
        else break;
    }

    int cx = left + 24;
    int width = 1280 - left - 48;
    if (cover_) {
        ui_.image(cover_, left + 40, 150, 300, 300);
        cx = left + 370;
        width = 1280 - cx - 40;
    }
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
