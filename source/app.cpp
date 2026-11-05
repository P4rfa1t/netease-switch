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

// Palette lifted from the official TV client's values/colors.xml
// (t_common_tab_*, t_white_*, t_btn_setting_*).
const uint32_t kBg = rgba(18, 18, 20);
const uint32_t kSidebarBg = rgba(24, 24, 26);
const uint32_t kPanelBg = rgba(30, 30, 32);
const uint32_t kAccent = rgba(255, 58, 58);       // t_common_dialog_btn_primary
const uint32_t kText = rgba(255, 255, 255);       // t_white_100
const uint32_t kTextDim = rgba(205, 205, 205);    // t_white_80
const uint32_t kSub = rgba(153, 153, 153);        // t_white_60
const uint32_t kHint = rgba(102, 102, 102);       // t_white_40
const uint32_t kCardBg = rgba(255, 255, 255, 10); // t_common_tab_bg_checked
const uint32_t kRowSelected = rgba(255, 58, 58, 46);
const uint32_t kWhite = rgba(255, 255, 255);

const int kBrValues[] = {128000, 192000, 320000, 999000};
const char *kBrNames[] = {"128K", "192K", "320K", "无损"};

constexpr int kTopBarHeight = 92;
constexpr int kGridTop = 168;
constexpr int kCardGap = 20;
constexpr int kCardW = 216;
constexpr int kCardH = 216;
constexpr int kCardsPerRow = 5;
constexpr int kCardTextH = 62;
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
    // The TV client encodes its identity into the scan-login URL. The phone shows
    // hdw_model as the device name, so passing it here is what makes the name
    // appear correctly (cookies alone are ignored by this page).
    qr_url_ = "https://music.163.com/st/platform/scanlogin?codekey=" + qr_key_ +
              "&hdw_deviceid=" + api_.deviceId() +
              "&hdw_device=switch" +
              "&hdw_brand=official" +
              "&hdw_model=NintendoSwitch" +
              "&hdw_ip=" +
              "&hdw_token=";
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
        logf("[login] authorized, MUSIC_U len=%d", (int)cookie.size());
        if (!cookie.empty()) api_.setMusicU(cookie);
        uint64_t uid = 0;
        std::string nick;
        bool ok = api_.account(uid, nick);
        logf("[login] account ok=%d uid=%llu nick=%s", (int)ok, (unsigned long long)uid,
             nick.c_str());
        if (ok) {
            uid_ = uid;
            nickname_ = nick;
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
    std::string avatar;
    vip_text_.clear();
    bool ok = api_.account(uid_, nickname_, &avatar, &vip_text_);
    if (ok && !avatar.empty()) {
        avatar_url_ = avatar;
        requestCover(avatar);
        avatar_ = ui_.cachedImage(avatar);
    }
    return ok;
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

    loadDiscover();
    api_.homeBlocks(home_blocks_);
    for (const auto &b : home_blocks_) requestCover(b.cover);

    // Prefetch so the "我的" tiles and the player pill already have artwork.
    if (recent_.empty()) api_.recentSongs(recent_);
    if (toplists_.empty()) api_.toplists(toplists_);
    for (const auto &song : recent_) requestCover(song.cover_url);
    for (const auto &pl : toplists_) requestCover(pl.cover_url);
    for (const auto &pl : playlists_) requestCover(pl.cover_url);
    for (const auto &pl : discover_playlists_) requestCover(pl.cover_url);
    for (const auto &song : daily_) requestCover(song.cover_url);
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

void App::requestLogout() {
    if (confirm_logout_ && clockSeconds() <= logout_confirm_until_) {
        logout();
        return;
    }
    confirm_logout_ = true;
    logout_confirm_until_ = clockSeconds() + 3.0;
    toast("再按一次确认退出账号");
}

void App::logout() {
    api_.logout();
    nickname_.clear();
    uid_ = 0;
    liked_ids_.clear();
    liked_ids_loaded_ = false;
    liked_playlist_id_ = 0;
    liked_track_count_ = 0;
    daily_.clear();
    playlists_.clear();
    recent_.clear();
    toplists_.clear();
    recommend_.clear();
    tracks_.clear();
    play_queue_.clear();
    playing_index_ = -1;
    player_.stop();
    current_song_id_ = 0;
    cover_ = nullptr;
    cover_key_.clear();
    lyrics_.clear();
    show_lyrics_ = false;
    confirm_logout_ = false;
    logout_confirm_until_ = 0;
    tab_ = 0;
    logf("[login] logged out, credentials cleared");
    startLogin();
    toast("已退出账号");
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
    // Sidebar no longer exists in the TV layout; kept as a no-op for old hooks.
}

int App::sidebarWidth() const {
    return 0;
}

int App::contentX() const {
    return 56;
}

int App::contentWidth() const {
    return 1280 - 112;
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

    // Re-fetch the current track so the new level actually takes effect.
    if (current_song_id_ != 0 && !play_queue_.empty() && playing_index_ >= 0 &&
        playing_index_ < (int)play_queue_.size()) {
        const Song &song = play_queue_[playing_index_];
        std::string url;
        std::string error;
        if (api_.songUrl(song.id, kBrValues[quality_index_], url, error)) {
            player_.play(url, song.name, song.artist);
        } else {
            toast(error);
        }
    }
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
    tracks_cover_.clear();
    for (const auto &pl : playlists_) {
        if (pl.id == id) { tracks_cover_ = pl.cover_url; break; }
    }
    for (const auto &pl : discover_playlists_) {
        if (pl.id == id) { tracks_cover_ = pl.cover_url; break; }
    }
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

namespace {

std::vector<std::pair<double, std::string>> parseLrc(const std::string &lrc) {
    std::vector<std::pair<double, std::string>> out;
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
                size_t flen = strlen(fraction);
                double frac = flen ? atoi(fraction) / std::pow(10.0, (double)flen) : 0.0;
                offset = minutes * 60.0 + seconds + frac;
                has_tag = true;
                cursor = close + 1;
            } else {
                break;
            }
        }

        if (!has_tag) continue;
        std::string text = line.substr(cursor);
        if (text.empty() || text[0] == '{') continue;
        out.push_back({offset, text});
    }
    return out;
}

} // namespace

void App::loadLyrics(uint64_t song_id) {
    lyrics_.clear();
    lyrics_song_ = song_id;

    std::string lrc;
    std::string translated;
    if (!api_.lyric(song_id, lrc, translated)) return;

    auto main_lines = parseLrc(lrc);
    auto sub_lines = parseLrc(translated);

    for (const auto &line : main_lines) {
        LyricLine entry;
        entry.time = line.first;
        entry.text = line.second;
        for (const auto &sub : sub_lines) {
            if (std::fabs(sub.first - line.first) < 0.35 && sub.second != line.second) {
                entry.translation = sub.second;
                break;
            }
        }
        lyrics_.push_back(entry);
    }

    std::sort(lyrics_.begin(), lyrics_.end(),
              [](const LyricLine &a, const LyricLine &b) { return a.time < b.time; });
    logf("[lyrics] song=%llu lines=%d translated=%d", (unsigned long long)song_id,
         (int)lyrics_.size(), (int)sub_lines.size());
}

void App::toast(const std::string &message) {
    toast_text_ = message;
    toast_until_ = clockSeconds() + 3.0;
    logf("[toast] %s", message.c_str());
}

bool App::pressedRepeat(uint64_t button) {
    if (!(held_ & button)) {
        setRepeatNext((HidNpadButton)button, 0.0);
        return false;
    }
    double now = clockSeconds();
    if (down_ & button) {
        setRepeatNext((HidNpadButton)button, now + 0.45);
        return true;
    }
    if (now >= repeat_next((HidNpadButton)button)) {
        setRepeatNext((HidNpadButton)button, now + 0.10);
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
    // ---------- login ----------
    if (screen_ == Screen::Login) {
        if (x >= 520 && x <= 760 && y >= 600 && y <= 648 &&
            clockSeconds() >= refresh_allowed_at_) {
            startLogin();
        }
        return;
    }

    // ---------- player page ----------
    if (screen_ == Screen::Player) {
        // top bar: back to home
        if (y < kTopBarHeight) {
            if (x < 360) { screen_ = Screen::Home; return; }
            // tab area
            if (x >= 460 && x < 610) { switchPage(Page::Recommend); screen_ = Screen::Home; return; }
            if (x >= 610 && x < 760) { switchPage(Page::Mine); screen_ = Screen::Home; return; }
            return;
        }
        // progress bar seek
        if (y >= 592 && y <= 620 && x >= 120 && x <= 1160) {
            double total = player_.duration();
            if (total > 0) player_.seek(((x - 120.0) / 1040.0) * total);
            return;
        }
        // controls
        if (y >= 650) {
            if (x >= 150 && x <= 220) { player_.togglePause(); return; }
            if (x >= 520 && x <= 600) { playAdjacent(-1); return; }
            if (x >= 680 && x <= 760) { playAdjacent(1); return; }
            if (x >= 1000) { screen_ = Screen::Home; return; }
            return;
        }
        return;
    }

    // ---------- top bar on home/playlist ----------
    if (y < kTopBarHeight) {
        // now-playing pill -> player page
        if (x < 360) {
            if (current_song_id_ != 0) screen_ = Screen::Player;
            return;
        }
        if (x >= 460 && x < 610) { switchPage(Page::Recommend); return; }
        if (x >= 610 && x < 760) { switchPage(Page::Mine); return; }
        if (x >= 1100) { startSearch(); return; }
        return;
    }

    // ---------- playlist detail ----------
    if (screen_ == Screen::Playlist) {
        // action pills
        if (y >= 150 && y <= 196) {
            if (x >= 640 && x < 640 + 140) {  // 播放全部
                if (!tracks_.empty()) {
                    play_queue_ = tracks_;
                    playing_index_ = 0;
                    playSong(tracks_[0]);
                }
                return;
            }
        }
        // two-column list: 4 rows x 2 cols
        if (y >= 316) {
            const int rows_per_page = 4;
            int total_pages = ((int)tracks_.size() + rows_per_page * 2 - 1) / (rows_per_page * 2);
            int page = total_pages > 0 ? track_sel_ / (rows_per_page * 2) : 0;
            int page_start = page * rows_per_page * 2;

            int col = (x >= 596) ? 1 : 0;
            int row = (y - 316) / 84;
            if (row >= 0 && row < rows_per_page) {
                int index = page_start + col * rows_per_page + row;
                if (index >= 0 && index < (int)tracks_.size()) {
                    track_sel_ = index;
                    play_queue_ = tracks_;
                    playing_index_ = index;
                    playSong(tracks_[index]);
                    return;
                }
            }
        }
        // pagination
        if (y >= 674 && y <= 700) {
            int total_pages = ((int)tracks_.size() + 7) / 8;
            int page = track_sel_ / 8;
            if (x >= 580 && x <= 610 && page > 0) {
                track_sel_ = std::max(0, (page - 1) * 8);
            } else if (x >= 670 && x <= 700 && page + 1 < total_pages) {
                track_sel_ = std::min((int)tracks_.size() - 1, (page + 1) * 8);
            }
            track_scroll_ = track_sel_ / 8;
            return;
        }
        return;
    }

    // ---------- home ----------
    if (page_ == Page::Recommend) {
        // four feature cards
        if (y >= 186 && y <= 486) {
            int idx = (int)((x - 56) / 308);
            if (idx >= 0 && idx < 4) {
                card_index_ = idx;
                nav_focused_ = false;
                activateCard();
                return;
            }
        }
        // recommended playlist row
        if (y >= 606 && y <= 700) {
            int idx = (int)((x - 56) / 238);
            if (idx >= 0 && idx < (int)discover_playlists_.size()) {
                const Playlist &p = discover_playlists_[idx];
                openPlaylist(p.id, p.name);
                return;
            }
        }
        return;
    }

    if (show_playlist_grid_) {
        // section tabs
        if (y >= 108 && y <= 166) {
            if (x >= 468 && x <= 620) { mine_section_ = 0; mine_focus_ = 0; mine_sel_ = 0; return; }
            if (x >= 620 && x <= 800) { mine_section_ = 1; mine_focus_ = 0; mine_sel_ = 0; return; }
            return;
        }
        if (y >= 200) {
            const int cols = 6, card_w = 180, gap = 22;
            int c = (int)((x - 56) / (card_w + gap));
            int r = (int)((y - 200) / 254);
            if (c >= 0 && c < cols && r >= 0 && r < 2) {
                int index = (mine_scroll_ + r) * cols + c;
                std::vector<Playlist> items = mineSectionPlaylists();
                if (index >= 0 && index < (int)items.size()) {
                    mine_sel_ = index;
                    openPlaylist(items[index].id, items[index].name);
                    show_playlist_grid_ = false;
                }
            }
        }
        return;
    }

    if (page_ == Page::Mine) {
        // logout button
        if (y >= 190 && y <= 242 && x >= 1000 && x <= 1170) {
            requestLogout();
            return;
        }
        // Six entry tiles: 3 columns x 2 rows, 280x156 each.
        if (y >= 366 && y <= 700) {
            int col = (int)((x - 56) / 300);
            int row = (y >= 542) ? 1 : 0;
            if (col >= 0 && col < 3) {
                int idx = row * 3 + col;
                if (idx >= 0 && idx < 6) {
                    card_index_ = idx;
                    nav_focused_ = false;
                    activateCard();
                }
            }
        }
        return;
    }
}

void App::handleDrag(float, float dy) {
    if (screen_ == Screen::Login || show_lyrics_) return;
    if (touch_start_y_ < kTopBarHeight) return;

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

    if (pressed(HidNpadButton_X)) player_.togglePause();
    if (pressed(HidNpadButton_L)) playAdjacent(-1);
    if (pressed(HidNpadButton_R)) playAdjacent(1);
    if (pressedRepeat(HidNpadButton_ZL)) player_.adjustVolume(-0.05f);
    if (pressedRepeat(HidNpadButton_ZR)) player_.adjustVolume(0.05f);
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
    if (pressed(HidNpadButton_Y)) {
        screen_ = (screen_ == Screen::Player) ? Screen::Home : Screen::Player;
    }

    if (pressed(HidNpadButton_B)) {
        if (screen_ == Screen::Playlist) screen_ = Screen::Home;
        else if (screen_ == Screen::Player) screen_ = Screen::Home;
        else if (!nav_focused_) nav_focused_ = true;
        else { nav_index_ = 0; switchPage(Page::Recommend); }
    }

    if (screen_ == Screen::Login) {
        if (pressed(HidNpadButton_A) && clockSeconds() >= refresh_allowed_at_) startLogin();
        return;
    }

    if (screen_ == Screen::Player) {
        if (pressed(HidNpadButton_Left)) player_.seek(player_.position() - 10.0);
        if (pressed(HidNpadButton_Right)) player_.seek(player_.position() + 10.0);
        if (pressed(HidNpadButton_A)) player_.togglePause();
        return;
    }

    if (screen_ == Screen::Playlist) {
        // 4 rows x 2 columns per page
        const int rows = 4;
        int count = (int)tracks_.size();
        if (count == 0) return;

        int col = track_sel_ / rows;      // which column this index sits in
        int row = track_sel_ % rows;
        int per_page = rows * 2;
        int page = track_sel_ / per_page;

        auto go = [&](int new_row, int new_col, int new_page) {
            new_page = std::clamp(new_page, 0, (count - 1) / per_page);
            int local = new_col * rows + new_row;
            int idx = new_page * per_page + local;
            if (idx >= 0 && idx < count) track_sel_ = idx;
            track_scroll_ = track_sel_ / per_page;
        };

        if (pressedRepeat(HidNpadButton_Up)) {
            if (row > 0) go(row - 1, col, page);
        }
        if (pressedRepeat(HidNpadButton_Down)) {
            if (row < rows - 1) go(row + 1, col, page);
        }
        if (pressedRepeat(HidNpadButton_Left)) {
            if (col == 1) go(row, 0, page);
            else go(row, 1, page - 1);
        }
        if (pressedRepeat(HidNpadButton_Right)) {
            if (col == 0) go(row, 1, page);
            else go(row, 0, page + 1);
        }

        if (pressed(HidNpadButton_A)) playSelectedFromPlaylist();
        return;
    }

    if (show_playlist_grid_) {
        int total = (int)mineSectionPlaylists().size();
        const int cols = 6;

        if (mine_focus_ == 0) {
            if (pressed(HidNpadButton_Left)) mine_section_ = 0;
            if (pressed(HidNpadButton_Right)) mine_section_ = 1;
            if (pressed(HidNpadButton_Down)) mine_focus_ = 1;
            if (pressed(HidNpadButton_A)) mine_focus_ = 1;
            return;
        }

        if (pressed(HidNpadButton_Up)) {
            if (mine_sel_ < cols) mine_focus_ = 0;
            else mine_sel_ -= cols;
        }
        if (pressedRepeat(HidNpadButton_Down)) {
            if (mine_sel_ + cols < total) mine_sel_ += cols;
        }
        if (pressedRepeat(HidNpadButton_Left) && mine_sel_ > 0) --mine_sel_;
        if (pressedRepeat(HidNpadButton_Right) && mine_sel_ < total - 1) ++mine_sel_;

        int rows_visible = 2;
        int row = mine_sel_ / cols;
        mine_scroll_ = std::clamp(mine_scroll_, std::max(0, row - rows_visible + 1),
                                  std::min(row, std::max(0, (total + cols - 1) / cols - rows_visible)));

        if (pressed(HidNpadButton_A)) {
            std::vector<Playlist> items = mineSectionPlaylists();
            if (mine_sel_ < (int)items.size()) {
                openPlaylist(items[mine_sel_].id, items[mine_sel_].name);
                show_playlist_grid_ = false;
            }
        }
        if (pressed(HidNpadButton_B)) {
            show_playlist_grid_ = false;
            screen_ = Screen::Home;
        }
        return;
    }

    if (nav_focused_) {
        if (pressed(HidNpadButton_Left) && nav_index_ > 0) --nav_index_;
        if (pressed(HidNpadButton_Right) && nav_index_ < 1) ++nav_index_;
        if (pressed(HidNpadButton_A)) {
            switchPage(nav_index_ == 0 ? Page::Recommend : Page::Mine);
            nav_focused_ = false;
        }
        if (pressed(HidNpadButton_Down)) nav_focused_ = false;
        return;
    }

    const int cols = (page_ == Page::Mine) ? 3 : 4;
    const int rows = (page_ == Page::Mine) ? 2 : 1;
    const int total = cols * rows;

    // Moving up from the first row returns to the tab bar.
    if (pressed(HidNpadButton_Up)) {
        if (card_index_ < cols) {
            nav_focused_ = true;
            return;
        }
        card_index_ -= cols;
        return;
    }
    if (pressed(HidNpadButton_Down) && card_index_ + cols < total) {
        card_index_ += cols;
        return;
    }
    if (pressedRepeat(HidNpadButton_Left) && card_index_ % cols > 0) --card_index_;
    if (pressedRepeat(HidNpadButton_Right) && card_index_ % cols < cols - 1) ++card_index_;
    if (pressed(HidNpadButton_A)) activateCard();
}

void App::update() {
    pollLogin();
    if (!pending_covers_.empty()) pollCovers();
    if (!avatar_url_.empty() && !avatar_) avatar_ = ui_.cachedImage(avatar_url_);

    if (confirm_logout_ && clockSeconds() > logout_confirm_until_) {
        confirm_logout_ = false;
    }

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
    if (drawing_) return;   // never re-enter through a draw-time callback
    draw();
    SDL_RenderPresent(renderer_);
}

void App::draw() {
    drawing_ = true;
    ui_.clear(kBg);

    switch (screen_) {
    case Screen::Login:
        drawLogin();
        break;
    case Screen::Home:
        if (show_playlist_grid_) drawMineGrid();
        else drawHome();
        break;
    case Screen::Playlist:
        drawPlaylist();
        drawNowBar();
        break;
    case Screen::Player:
        drawPlayerPage();
        break;
    }

    if (screen_ == Screen::Player) drawPlayerPage();
    if (show_lyrics_) drawLyrics();
    drawToast();
    drawing_ = false;
}

void App::drawTopBar() {
    ui_.rect(0, 0, 1280, kTopBarHeight, rgba(12, 12, 14, 235));
    ui_.rect(0, kTopBarHeight - 1, 1280, 1, rgba(255, 255, 255, 14));

    // Now-playing pill on the left.
    ui_.rect(20, 16, 330, 60, rgba(255, 255, 255, 16));
    if (cover_) ui_.image(cover_, 30, 24, 44, 44);
    int tx = cover_ ? 84 : 36;
    ui_.text(tx, 24, utf8Truncate(player_.title(), 16), 17, kText);
    ui_.text(tx, 48, utf8Truncate(player_.artist(), 18), 13, kSub);

    // Only two tabs: 推荐 / 我的.
    const char *tabs[] = {"推荐", "我的"};
    const Page pages[] = {Page::Recommend, Page::Mine};
    int x = 460;
    for (int i = 0; i < 2; ++i) {
        bool active = page_ == pages[i];
        ui_.text(x, 32, tabs[i], 24, active ? kText : kSub);
        if (active) {
            ui_.rect(x + 6, 66, ui_.textWidth(tabs[i], 24) - 12, 3, kAccent);
        }
        if (nav_focused_ && nav_index_ == i) {
            ui_.rectOutline(x - 18, 20, ui_.textWidth(tabs[i], 24) + 36, 46,
                            rgba(255, 255, 255, 60), 1);
        }
        x += 150;
    }
}

void App::activateCard() {
    if (page_ == Page::Recommend) {
        if (card_index_ == 0) {
            screen_ = Screen::Player;
        } else if (card_index_ == 2) {
            if (!daily_.empty()) {
                play_queue_ = daily_;
                playing_index_ = 0;
                playSong(daily_[0]);
                screen_ = Screen::Player;
            } else {
                toast("每日推荐为空");
            }
        } else if (card_index_ == 3) {
            if (!recommend_.empty()) {
                play_queue_ = recommend_.empty() ? daily_ : recent_;
                playSong(play_queue_.empty() ? Song{} : play_queue_[0]);
            } else {
                toast("私人漫游暂未开放");
            }
        } else {
            if (!discover_playlists_.empty()) {
                openPlaylist(discover_playlists_[0].id, discover_playlists_[0].name);
            } else {
                loadDiscover();
            }
        }
    } else if (page_ == Page::Mine) {
        if (card_index_ == 0) {
            loadRecent();
            if (!recent_.empty()) {
                tracks_ = recent_;
                tracks_title_ = "最近播放";
                tracks_cover_ = recent_[0].cover_url;
                track_sel_ = 0;
                play_queue_ = recent_;
                screen_ = Screen::Playlist;
            } else {
                toast("最近播放为空");
            }
        } else if (card_index_ == 1) {
            if (liked_playlist_id_ != 0) openPlaylist(liked_playlist_id_, "我喜欢的音乐");
            else toast("未找到我喜欢的音乐");
        } else if (card_index_ == 2) {
            if (playlists_.empty()) {
                std::vector<Playlist> lists;
                if (api_.userPlaylists(lists)) playlists_ = lists;
            }
            if (playlists_.empty()) toast("歌单加载失败");
            else { show_playlist_grid_ = true; mine_section_ = 0; mine_sel_ = 0; mine_focus_ = 0; }
        } else if (card_index_ == 3) {
            if (toplists_.empty()) loadToplists();
            if (!toplists_.empty()) {
                tracks_.clear();
                tracks_title_ = "排行榜";
                show_toplists_ = true;
                top_sel_ = 0;
            } else {
                toast("排行榜加载失败");
            }
        } else if (card_index_ == 4) {
            toast("关注歌手需要额外权限，当前会话不可用");
        } else {
            toast("收藏视频暂未实现");
        }
    }

}

void App::switchPage(Page page) {
    page_ = page;
    card_index_ = 0;
    card_scroll_ = 0;
    nav_focused_ = false;

    if (page == Page::Mine && playlists_.empty() && uid_ != 0) {
        std::vector<Playlist> lists;
        if (api_.userPlaylists(lists)) {
            playlists_ = lists;
            for (const auto &p : playlists_) {
                if (p.special_type == 5) {
                    liked_playlist_id_ = p.id;
                    liked_track_count_ = p.track_count;
                    break;
                }
            }
        }
    }
}

void App::loadDiscover() {
    redrawNow();
    std::vector<Playlist> lists;
    if (api_.recommendPlaylists(lists)) {
        discover_playlists_ = lists;
    } else {
        toast("发现页加载失败");
    }
}

void App::requestCover(const std::string &url) {
    if (url.empty()) return;
    if (ui_.cachedImage(url)) return;
    if (pending_covers_.count(url)) return;
    pending_covers_.insert(url);
}

void App::pollCovers() {
    // Cap per frame: covers are 100-200 KB each and the UI thread must stay responsive.
    int budget = 4;
    auto it = pending_covers_.begin();
    while (it != pending_covers_.end() && budget-- > 0) {
        const std::string url = *it;
        it = pending_covers_.erase(it);
        std::vector<unsigned char> bytes;
        if (http_.download(url, bytes)) {
            ui_.textureFromMemory(url, bytes);
        }
    }
}

SDL_Texture *App::iconTexture(const char *name) {
    auto it = icons_.find(name);
    if (it != icons_.end()) return it->second;

    std::string path = std::string("romfs:/icons/") + name + ".png";
    std::vector<unsigned char> bytes;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        logf("[ui] icon missing: %s", path.c_str());
        icons_[name] = nullptr;
        return nullptr;
    }
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    fclose(f);

    SDL_Texture *tex = ui_.textureFromMemory(path, bytes);
    icons_[name] = tex;
    return tex;
}

void App::drawIcon(const char *name, int x, int y, int w, int h) {
    SDL_Texture *tex = iconTexture(name);
    if (tex) {
        ui_.image(tex, x, y, w, h);
    } else {
        ui_.rect(x, y, w, h, rgba(255, 255, 255, 40));
    }
}

void App::drawCover(const std::string &url, int x, int y, int w, int h) {
    SDL_Texture *tex = ui_.cachedImage(url);
    if (tex) {
        ui_.image(tex, x, y, w, h);
        return;
    }
    ui_.rect(x, y, w, h, rgba(255, 255, 255, 18));
    if (!url.empty()) requestCover(url);
}

void App::drawPlaylistGrid(const std::vector<Playlist> &cards, int selected, int scroll,
                           int top, const char *empty_hint) {
    if (cards.empty()) {
        ui_.text(60, top + 40, empty_hint, 20, kHint);
        return;
    }

    int total_rows = (int)((cards.size() + kCardsPerRow - 1) / kCardsPerRow);
    int visible_rows = std::max(1, (720 - top - 120) / (kCardH + kCardTextH + kCardGap));
    int first_row = std::clamp(scroll, 0, std::max(0, total_rows - visible_rows));

    for (int r = 0; r < visible_rows; ++r) {
        for (int c = 0; c < kCardsPerRow; ++c) {
            int index = (first_row + r) * kCardsPerRow + c;
            if (index >= (int)cards.size()) break;

            int x = 60 + c * (kCardW + kCardGap);
            int y = top + r * (kCardH + kCardTextH + kCardGap);
            const Playlist &p = cards[index];
            bool sel = index == selected;

            drawCover(p.cover_url, x, y, kCardW, kCardH);

            if (sel) {
                ui_.rectOutline(x - 4, y - 4, kCardW + 8, kCardH + 8, kAccent, 3);
            }

            ui_.text(x, y + kCardH + 10, utf8Truncate(p.name, 14), 17,
                     sel ? kText : kTextDim);
            if (p.track_count > 0) {
                char meta[32];
                snprintf(meta, sizeof(meta), "%d 首", p.track_count);
                ui_.text(x, y + kCardH + 34, meta, 13, kHint);
            }
        }
    }
}

void App::drawPlayerPage() {
    ui_.rect(0, 0, 1280, 720, rgba(14, 14, 16));
    drawTopBar();

    // --- vinyl disc, left half ---
    int cx = 340, cy = 400, r = 210;
    drawIcon("disc", cx - r - 8, cy - r - 8, (r + 8) * 2, (r + 8) * 2);
    drawCover(cover_key_, cx - 118, cy - 118, 236, 236);
    ui_.circle(cx, cy, 10, rgba(16, 16, 18));

    // tonearm
    ui_.rect(cx + 150, cy - r - 30, 8, 120, rgba(210, 210, 210));
    ui_.circle(cx + 154, cy - r - 34, 10, rgba(235, 235, 235));
    ui_.rect(cx + 130, cy - r + 86, 66, 7, rgba(210, 210, 210));

    // --- info, right half ---
    int tx = 660;
    ui_.text(tx, 250, utf8Truncate(player_.title(), 20), 40, kText);
    ui_.text(tx, 310, utf8Truncate(player_.artist(), 24), 20, kSub);
    ui_.text(tx, 350, kBrNames[quality_index_], 16, rgba(255, 255, 255, 140));

    // --- progress ---
    double pos = player_.position();
    double dur = player_.duration();
    double ratio = dur > 0 ? std::clamp(pos / dur, 0.0, 1.0) : 0.0;

    int bar_x = 140, bar_w = 1000, bar_y = 592;
    ui_.rect(bar_x, bar_y, bar_w, 4, rgba(255, 255, 255, 40));
    ui_.rect(bar_x, bar_y, (int)(bar_w * ratio), 4, kAccent);
    ui_.circle(bar_x + (int)(bar_w * ratio), bar_y + 2, 6, kText);
    ui_.text(bar_x, bar_y + 14, formatTime(pos), 15, kSub);
    ui_.textRight(bar_x + bar_w, bar_y + 14, formatTime(dur), 15, kSub);

    // --- controls using the official TV client icons ---
    int cy2 = 660;
    drawIcon("prev", 520, cy2 - 24, 48, 48);

    bool paused = player_.state() == Player::State::Paused ||
                  player_.state() == Player::State::Idle;
    if (paused) {
        drawIcon("play", 616, cy2 - 28, 56, 56);
    } else {
        ui_.pauseBars(620, cy2 - 22, 40, 44, kText);
    }

    drawIcon("next", 704, cy2 - 24, 48, 48);

    // side actions
    bool liked = current_song_id_ != 0 && liked_ids_.count(current_song_id_) != 0;
    drawIcon("like", 140, cy2 - 20, 40, 40);
    if (liked) ui_.rectOutline(136, cy2 - 24, 48, 48, kAccent, 2);

    ui_.text(200, cy2 - 8, play_mode_ == PlayMode::Shuffle ? "随机"
                              : play_mode_ == PlayMode::RepeatOne ? "单曲"
                                                                  : "顺序",
             17, kSub);
    ui_.textRight(1140, cy2 - 10, "歌词", 17, kSub);
}

void App::drawTrackRow(const Song &song, int index, int y, bool selected) {
    int x = contentX();
    int w = contentWidth();

    if (selected) {
        ui_.rect(x, y, w, 52, kRowSelected);
        ui_.rect(x, y, 3, 52, kAccent);
        ui_.rectOutline(x, y, w, 52, rgba(255, 58, 58, 90), 1);
    }

    // Index column dims when the row is selected so the title stays dominant.
    char number[16];
    snprintf(number, sizeof(number), "%d", index + 1);
    ui_.textRight(x + 46, y + 16, number, 15, selected ? kTextDim : kHint);

    int text_width = w - 190;
    ui_.text(x + 66, y + 5, utf8Truncate(song.name, (size_t)(text_width / 19)), 19,
             selected ? kText : kTextDim);
    ui_.text(x + 66, y + 29, utf8Truncate(song.artist, (size_t)(text_width / 14)), 14, kSub);

    ui_.textRight(x + w - 22, y + 16, formatDuration(song.duration_ms), 15, kHint);
}

void App::drawScrollbar(int x, int y, int height, int visible, int total, int first) {
    if (total <= visible) return;
    int bar_height = std::max(24, height * visible / total);
    int max_first = std::max(1, total - visible);
    int offset = (height - bar_height) * first / max_first;
    ui_.rect(x, y + offset, 4, bar_height, rgba(90, 90, 100));
}

std::vector<Playlist> App::mineSectionPlaylists() {
    if (mine_section_ == 1) {
        // Collected playlists come from their own endpoint; the merged
        // user/playlist result does not reliably flag subscribed.
        if (collected_.empty() && !collected_loaded_) {
            // No redraw here: this runs inside drawMineGrid(), and redrawing
            // would re-enter the same function.
            std::vector<Playlist> lists;
            if (api_.collectedPlaylists(lists)) collected_ = lists;
            collected_loaded_ = true;
        }
        return collected_;
    }

    std::vector<Playlist> out;
    for (const auto &p : playlists_) {
        if (p.special_type == 5) continue;      // "我喜欢的音乐" has its own entry
        if (p.subscribed) continue;             // belongs to the collected tab
        out.push_back(p);
    }
    return out;
}

void App::drawMineGrid() {
    ui_.rect(0, 0, 1280, 720, kBg);
    drawTopBar();

    // Section tabs: 创建的歌单 / 收藏的歌单
    const char *tabs[2] = {"创建的歌单", "收藏的歌单"};
    int tx = 480;
    for (int i = 0; i < 2; ++i) {
        bool active = mine_section_ == i;
        ui_.text(tx, 118, tabs[i], 26, active ? kAccent : kTextDim);
        if (active) ui_.rect(tx, 158, ui_.textWidth(tabs[i], 26), 3, kAccent);
        if (mine_focus_ == 0 && mine_section_ == i) {
            ui_.rectOutline(tx - 12, 108, ui_.textWidth(tabs[i], 26) + 24, 58,
                            rgba(255, 255, 255, 70), 1);
        }
        tx += ui_.textWidth(tabs[i], 26) + 70;
    }

    std::vector<Playlist> items = mineSectionPlaylists();
    if (items.empty()) {
        ui_.textCentered(640, 360, "暂无歌单", 20, kHint);
        return;
    }

    const int cols = 6, card_w = 180, card_h = 180, gap = 22;
    int x0 = 56, y0 = 200;
    int rows_visible = 2;
    int first_row = std::clamp(mine_scroll_, 0,
                               std::max(0, (int)((items.size() + cols - 1) / cols) - rows_visible));

    for (int r = 0; r < rows_visible; ++r) {
        for (int c = 0; c < cols; ++c) {
            int index = (first_row + r) * cols + c;
            if (index >= (int)items.size()) break;
            int x = x0 + c * (card_w + gap);
            int y = y0 + r * (card_h + 74);
            const Playlist &p = items[index];

            drawCover(p.cover_url, x, y, card_w, card_h);
            ui_.rect(x, y + card_h - 54, card_w, 54, rgba(0, 0, 0, 140));
            ui_.text(x + 12, y + card_h - 44, utf8Truncate(p.name, 11), 16, kText);
            char meta[32];
            snprintf(meta, sizeof(meta), "%d 首", p.track_count);
            ui_.text(x + 12, y + card_h - 22, meta, 13, kSub);

            if (mine_focus_ == 1 && index == mine_sel_) {
                ui_.rectOutline(x - 4, y - 4, card_w + 8, card_h + 8, kAccent, 3);
            }
        }
    }
}

void App::drawHome() {
    ui_.rect(0, 0, 1280, 720, kBg);
    drawTopBar();

    if (page_ == Page::Recommend) {
        ui_.text(56, 118, "夜深了", 34, kText);

        // Cards come from homepage/get; fall back to the fixed four if empty.
        struct Fallback { const char *title; const char *desc; };
        const Fallback fb[4] = {
            {"继续播放", nullptr}, {"歌单推荐", nullptr},
            {"每日推荐", "符合你口味的新鲜好歌"}, {"私人漫游", "多种听歌模式随心播放"},
        };

        int y = 186, w = 286, h = 300, gap = 22, x0 = 56;
        int count = home_blocks_.empty() ? 4 : std::min((int)home_blocks_.size(), 4);

        for (int i = 0; i < count; ++i) {
            int x = x0 + i * (w + gap);

            std::string art;
            std::string title;
            std::string desc;
            if (home_blocks_.empty()) {
                if (i == 0) art = cover_key_;
                else if (i == 2) art = daily_.empty() ? "" : daily_[0].cover_url;
                title = fb[i].title;
                if (fb[i].desc) desc = fb[i].desc;
            } else {
                const HomeBlock &b = home_blocks_[i];
                art = b.cover;
                title = b.title;
                desc = b.subtitle;
            }

            drawCover(art, x, y, w, h);
            ui_.rect(x, y, w, h, rgba(0, 0, 0, 70));
            ui_.text(x + 20, y + 20, utf8Truncate(title, 10), 26, kText);
            if (!desc.empty()) {
                ui_.rect(x, y + h - 56, w, 56, rgba(0, 0, 0, 160));
                ui_.text(x + 20, y + h - 42, utf8Truncate(desc, 14), 15, kTextDim);
            }
            if (card_index_ == i && !nav_focused_) {
                ui_.rectOutline(x - 4, y - 4, w + 8, h + 8, kAccent, 3);
            }
        }

        ui_.text(56, 552, "你的专属推荐歌单", 28, kText);
        char hint[64];
        snprintf(hint, sizeof(hint), "共 %d 个", (int)discover_playlists_.size());
        ui_.textRight(1180, 560, hint, 15, kHint);

        for (int i = 0; i < 6; ++i) {
            int index = strip_scroll_ + i;
            if (index >= (int)discover_playlists_.size()) break;
            const Playlist &p = discover_playlists_[index];
            int x = 56 + i * 200;
            drawCover(p.cover_url, x, 600, 180, 180);
            if (i == 0 && discover_sel_ == index) {
                ui_.rectOutline(x - 3, 597, 186, 186, kAccent, 3);
            }
        }

    } else if (page_ == Page::Mine) {
        // Profile card: avatar on the left, nickname + UID to its right.
        ui_.rect(56, 118, 1168, 200, rgba(255, 255, 255, 14));

        if (avatar_) {
            ui_.image(avatar_, 92, 148, 132, 132);
            ui_.circle(0, 0, 0, kBg);  // no-op, keeps layout stable
        } else {
            ui_.circle(158, 214, 66, rgba(255, 255, 255, 30));
            ui_.textCentered(158, 190, "头像", 18, kHint);
        }

        ui_.text(256, 158, nickname_.empty() ? "未登录" : utf8Truncate(nickname_, 18), 34, kText);

        char uid_text[64];
        snprintf(uid_text, sizeof(uid_text), "UID %llu", (unsigned long long)uid_);
        ui_.text(256, 208, uid_text, 17, kSub);
        if (!vip_text_.empty()) {
            ui_.rect(256, 240, ui_.textWidth(vip_text_, 15) + 24, 30, rgba(230, 180, 90, 60));
            ui_.text(268, 245, vip_text_, 15, rgba(240, 200, 120));
        }

        ui_.rect(1000, 190, 170, 52, rgba(255, 255, 255, 30));
        ui_.textCentered(1085, 203, "退出登录", 17, kTextDim);

        const char *entries[6] = {"最近播放", "我喜欢的", "我的歌单",
                                  "排行榜", "关注歌手", "收藏视频"};
        for (int i = 0; i < 6; ++i) {
            int col = i % 3;
            int row = i / 3;
            int x = 56 + col * 300;
            int y = 366 + row * 176;
            int w = 280, h = 156;

            ui_.rect(x, y, w, h, rgba(255, 255, 255, 14));
            ui_.text(x + 24, y + 22, entries[i], 24, kText);

            std::string art;
            if (i == 0 && !recent_.empty()) art = recent_[0].cover_url;
            else if (i == 1 && liked_playlist_id_ != 0) {
                for (const auto &pl : playlists_) {
                    if (pl.id == liked_playlist_id_) { art = pl.cover_url; break; }
                }
            } else if (i == 3 && !toplists_.empty()) {
                art = toplists_[0].cover_url;
            }
            drawCover(art, x + 174, y + 18, 90, 90);

            if (card_index_ == i && !nav_focused_) {
                ui_.rectOutline(x - 4, y - 4, w + 8, h + 8, kAccent, 3);
            }
        }
    }
}

void App::drawPlaylist() {
    ui_.rect(0, 0, 1280, 720, kBg);
    drawTopBar();

    // ---- header block: cover + title + count + action buttons ----
    ui_.rect(0, kTopBarHeight, 1280, 190, rgba(255, 255, 255, 8));

    drawCover(tracks_cover_, 56, 112, 150, 150);
    ui_.text(230, 122, utf8Truncate(tracks_title_, 22), 32, kText);

    char count[64];
    snprintf(count, sizeof(count), "%d 首", (int)tracks_.size());
    ui_.text(230, 172, count, 17, kSub);

    // Action pills.
    const char *actions[] = {"播放全部", "心动模式", "氛围听"};
    int ax = 640;
    for (int i = 0; i < 3; ++i) {
        int w = ui_.textWidth(actions[i], 16) + 56;
        ui_.rect(ax, 150, w, 46, i == 0 ? rgba(255, 58, 58, 220) : rgba(255, 255, 255, 30));
        ui_.textCentered(ax + w / 2, 161, actions[i], 16, kText);
        ax += w + 18;
    }

    // ---- two-column track list ----
    if (tracks_.empty()) {
        ui_.text(56, 330, "暂无歌曲", 20, kHint);
        return;
    }

    const int rows_per_page = 4;          // 4 rows x 2 columns = 8 per page
    int total_pages = ((int)tracks_.size() + rows_per_page * 2 - 1) / (rows_per_page * 2);
    int page = track_sel_ / (rows_per_page * 2);
    int page_start = page * rows_per_page * 2;

    for (int i = 0; i < rows_per_page * 2; ++i) {
        int index = page_start + i;
        if (index >= (int)tracks_.size()) break;

        int col = i / rows_per_page;
        int row = i % rows_per_page;
        int x = 56 + col * 600;
        int y = 320 + row * 88;

        const Song &song = tracks_[index];
        bool sel = index == track_sel_;

        if (sel) {
            ui_.rect(x - 14, y - 6, 580, 80, rgba(255, 58, 58, 40));
        }

        // Index number on the left, dimmed.
        char num[16];
        snprintf(num, sizeof(num), "%d", index + 1);
        ui_.textRight(x + 12, y + 26, num, 20, sel ? kText : kHint);

        ui_.text(x + 34, y + 4, utf8Truncate(song.name, 22), 24, sel ? kText : kTextDim);
        ui_.text(x + 34, y + 40, utf8Truncate(song.artist, 26), 15, kSub);
    }

    // ---- pagination ----
    char page_text[48];
    snprintf(page_text, sizeof(page_text), "%d / %d", page + 1, total_pages > 0 ? total_pages : 1);
    ui_.textCentered(640 - 30, 686, "<", 20, kSub);
    ui_.textCentered(640, 686, page_text, 18, kSub);
    ui_.textCentered(640 + 30, 686, ">", 20, kSub);
}

void App::drawNowBar() {
    ui_.rect(0, kNowBarY, 1280, 720 - kNowBarY, kPanelBg);
    ui_.rect(0, kNowBarY, 1280, 1, rgba(255, 255, 255, 16));

    if (player_.state() == Player::State::Idle) {
        ui_.text(32, kNowBarY + 34, "未在播放", 17, kHint);
        ui_.textRight(1248, kNowBarY + 34, "选择一首歌开始", 14, kHint);
        return;
    }

    bool paused = player_.state() == Player::State::Paused;
    bool loading = player_.state() == Player::State::Loading;

    ui_.rect(24, 644, 84, 52, (paused || loading) ? kCardBg : kAccent);
    ui_.textCentered(66, 659, paused ? "播放" : "暂停", 18, kText);

    if (cover_) {
        ui_.image(cover_, 124, 646, 56, 56);
        ui_.rectOutline(124, 646, 56, 56, rgba(255, 255, 255, 24), 1);
    }
    int text_x = cover_ ? 194 : 124;

    ui_.text(text_x, kNowBarY + 11, utf8Truncate(player_.title(), cover_ ? 17 : 23), 19, kText);
    ui_.text(text_x, kNowBarY + 40, utf8Truncate(player_.artist(), cover_ ? 21 : 27), 14, kSub);

    const char *state_text = "";
    switch (player_.state()) {
    case Player::State::Loading: state_text = "缓冲中"; break;
    case Player::State::Playing: state_text = "正在播放"; break;
    case Player::State::Paused: state_text = "已暂停"; break;
    case Player::State::Error: state_text = "播放失败"; break;
    case Player::State::Finished: state_text = "播放结束"; break;
    default: break;
    }
    const char *mode_text = play_mode_ == PlayMode::RepeatOne ? "单曲循环"
                            : play_mode_ == PlayMode::Shuffle   ? "随机播放"
                                                                : "顺序播放";
    ui_.text(text_x, kNowBarY + 62, std::string(state_text) + " · " + mode_text, 13, kHint);

    double position = player_.position();
    double duration = player_.duration();
    double ratio = duration > 0 ? position / duration : 0.0;
    ratio = std::clamp(ratio, 0.0, 1.0);

    int bar_x = 420;
    int bar_w = 520;
    int bar_y = kNowBarY + 36;
    ui_.rect(bar_x, bar_y, bar_w, 4, rgba(255, 255, 255, 26));
    ui_.rect(bar_x, bar_y, (int)(bar_w * ratio), 4, kAccent);
    if (duration > 0) {
        int knob = bar_x + (int)(bar_w * ratio);
        ui_.rect(knob - 3, bar_y - 2, 6, 8, kText);
    }

    ui_.text(bar_x, bar_y + 12, formatTime(position), 13, kHint);
    ui_.textRight(bar_x + bar_w, bar_y + 12, formatTime(duration), 13, kHint);

    bool liked = current_song_id_ != 0 && liked_ids_.count(current_song_id_) != 0;
    ui_.rect(956, 646, 70, 48, liked ? rgba(255, 58, 58, 200) : kCardBg);
    ui_.textCentered(991, 660, liked ? "已喜欢" : "喜欢", 14, liked ? kText : kTextDim);

    ui_.rect(1032, 646, 70, 48, kCardBg);
    ui_.textCentered(1067, 661, kBrNames[quality_index_], 15, kTextDim);

    ui_.rect(1108, 646, 132, 48, kCardBg);
    ui_.textCentered(1174, 659, "歌词", 18, kTextDim);
}

void App::drawLyrics() {
    int left = 0;
    ui_.rect(left, 0, 1280 - left, 720, rgba(12, 12, 16, 242));
    ui_.text(left + 40, 92, "歌词", 26, kText);
    ui_.textRight(1240, 96, "Y / 点击返回", 16, kHint);

    if (lyrics_.empty()) {
        ui_.textCentered(640, 340, "暂无歌词", 22, kSub);
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

        if (!lyrics_[index].translation.empty()) {
            uint32_t sub_color = offset == 0 ? rgba(210, 210, 220) : rgba(100, 100, 112);
            int sub_size = offset == 0 ? 18 : 14;
            ui_.textWrapped(cx, y + (offset == 0 ? 32 : 26), width,
                            lyrics_[index].translation, sub_size, sub_color, line_height);
        }
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
