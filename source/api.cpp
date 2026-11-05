#include "api.hpp"

#include <cstdlib>
#include <exception>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "util.hpp"

namespace ntv {

namespace {

// nlohmann's value() throws on null; JSON from this API is full of nulls, and an
// uncaught exception aborts the process on libnx.
std::string jsonStr(const nlohmann::json &j, const char *key, const char *fallback = "") {
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

long long jsonInt(const nlohmann::json &j, const char *key, long long fallback = 0) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_number()) return fallback;
    return it->get<long long>();
}

bool jsonBool(const nlohmann::json &j, const char *key, bool fallback = false) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}



const char *kHost = "https://interface3.music.163.com";
const char *kAppVer = "1.1.80";
const char *kVersionCode = "1001080";
const char *kDeviceIdPath = "sdmc:/switch/netease-switch/device_id.txt";
const char *kMusicUPath = "sdmc:/switch/netease-switch/music_u.txt";

std::string joinArtists(const nlohmann::json &song) {
    std::string out;
    const char *keys[] = {"ar", "artists"};
    for (const char *key : keys) {
        if (!song.contains(key) || !song[key].is_array()) continue;
        for (const auto &artist : song[key]) {
            if (!out.empty()) out += " / ";
            out += jsonStr(artist, "name");
        }
        if (!out.empty()) return out;
    }
    return out;
}

std::string albumName(const nlohmann::json &song) {
    if (song.contains("al") && song["al"].is_object()) return jsonStr(song["al"], "name");
    if (song.contains("album") && song["album"].is_object()) return jsonStr(song["album"], "name");
    return "";
}

std::string coverUrl(const nlohmann::json &song) {
    if (song.contains("al") && song["al"].is_object()) return jsonStr(song["al"], "picUrl");
    if (song.contains("album") && song["album"].is_object()) return jsonStr(song["album"], "picUrl");
    return "";
}

std::string readFile(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string data;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    while (!data.empty() && (data.back() == '\n' || data.back() == '\r' || data.back() == ' ')) {
        data.pop_back();
    }
    return data;
}

void writeFile(const std::string &path, const std::string &value) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(value.data(), 1, value.size(), f);
    fclose(f);
}

} // namespace

NetEaseApi::NetEaseApi(HttpClient &http) : http_(http) {
    device_id_ = readFile(kDeviceIdPath);
    if (device_id_.empty()) {
        device_id_ = crypto::randomHex(16);
        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/netease-switch", 0777);
        writeFile(kDeviceIdPath, device_id_);
    }
    music_u_ = readFile(kMusicUPath);
    if (music_u_.empty()) music_u_ = http_.cookie("MUSIC_U");
    if (!music_u_.empty()) http_.setCookie("MUSIC_U", music_u_);
}

void NetEaseApi::setMusicU(const std::string &value) {
    music_u_ = value;
    http_.setCookie("MUSIC_U", value);
    writeFile(kMusicUPath, value);
}

void NetEaseApi::logout() {
    // Best effort: tell the server, then always clear local state.
    call("logout", nullptr);

    music_u_.clear();
    uid_ = 0;
    http_.setCookie("MUSIC_U", "");
    remove(kMusicUPath);
}

std::string NetEaseApi::cookieHeader() const {
    // The client URL-encodes the device model, so any space becomes "%20" and
    // the login screen shows it literally. Use a space-free model name.
    std::string out = "os=android; appver=" + std::string(kAppVer) +
                      "; versioncode=" + kVersionCode + "; deviceId=" + device_id_ +
                      "; osver=9; mobilename=NintendoSwitch; resolution=1920x1080; "
                      "channel=switch; packageType=official; sn=";
    if (!music_u_.empty()) out += "; MUSIC_U=" + music_u_;
    return out;
}

std::string NetEaseApi::headerJson() const {
    nlohmann::json header = {
        {"os", "android"},
        {"appver", kAppVer},
        {"versioncode", kVersionCode},
        {"deviceId", device_id_},
        {"osver", "9"},
        {"mobilename", "NintendoSwitch"},
        {"resolution", "1920x1080"},
        {"channel", "switch"},
        {"packageType", "official"},
        {"sn", ""},
        {"requestId", std::to_string((long long)clockSeconds() * 1000) + "_1"},
    };
    if (!music_u_.empty()) header["MUSIC_U"] = music_u_;
    return header.dump();
}

nlohmann::json NetEaseApi::call(const std::string &path, const nlohmann::json &body,
                                const nlohmann::json &query) {
    nlohmann::json payload = body.is_null() ? nlohmann::json::object() : body;

    // The TV client puts its parameters in both the URL query and the JSON body.
    std::string qs;
    if (query.is_object()) {
        for (auto it = query.begin(); it != query.end(); ++it) {
            std::string value = it.value().is_string() ? it.value().get<std::string>()
                                                       : it.value().dump();
            if (!qs.empty()) qs += "&";
            qs += urlEncode(it.key()) + "=" + urlEncode(value);
            payload[it.key()] = value;
        }
    }

    payload["header"] = nlohmann::json::parse(headerJson());
    payload["e_r"] = "true";

    std::string inner = "/api/" + path;
    std::string params = crypto::eapiEncrypt(inner, payload.dump());
    std::string url = std::string(kHost) + "/eapi/" + path;
    if (!qs.empty()) url += "?" + qs;

    HttpResult r = http_.postFormWithCookie(url, "params=" + params, cookieHeader());
    if (!r.ok()) {
        logf("[api] %s http=%ld", path.c_str(), r.status);
        return nlohmann::json::object();
    }

    std::string raw = r.body;
    std::string decoded;
    if (!raw.empty() && raw[0] != '{' && raw[0] != '[' && crypto::eapiDecrypt(raw, decoded)) {
        raw = decoded;
    }

    try {
        return nlohmann::json::parse(raw);
    } catch (const std::exception &e) {
        logf("[api] %s parse error: %s", path.c_str(), e.what());
        return nlohmann::json::object();
    }
}

Song NetEaseApi::parseSong(const nlohmann::json &j) {
    Song s;
    s.id = (uint64_t)jsonInt(j, "id");
    s.name = jsonStr(j, "name");
    s.artist = joinArtists(j);
    s.album = albumName(j);
    s.cover_url = coverUrl(j);
    s.duration_ms = (int)jsonInt(j, "dt");
    if (!s.duration_ms) s.duration_ms = (int)jsonInt(j, "duration");
    return s;
}

bool NetEaseApi::loggedIn() const {
    return !music_u_.empty();
}

bool NetEaseApi::account(uint64_t &uid, std::string &nickname, std::string *avatar,
                         std::string *vip) {
    auto j = call("nuser/account/get", nullptr);
    if (!j.contains("account") || j["account"].is_null()) return false;
    uid = (uint64_t)jsonInt(j["account"], "id");
    uid_ = uid;
    if (j.contains("profile") && j["profile"].is_object()) {
        const auto &p = j["profile"];
        nickname = jsonStr(p, "nickname");
        if (avatar) *avatar = jsonStr(p, "avatarUrl");
        if (vip) {
            int vip_type = (int)jsonInt(p, "vipType");
            if (vip_type > 0) *vip = "黑胶 VIP";
        }
    }
    return uid != 0;
}

bool NetEaseApi::qrCreate(std::string &unikey) {
    auto j = call("login/qrcode/unikey", {{"type", "2"}});
    if ((int)jsonInt(j, "code") != 200) return false;
    unikey = jsonStr(j, "unikey");
    return !unikey.empty();
}

int NetEaseApi::qrCheck(const std::string &unikey) {
    auto j = call("login/qrcode/client/login", {{"type", "2"}, {"key", unikey}});
    int code = (int)jsonInt(j, "code", -1);
    if (code == 803) {
        std::string cookie = http_.cookie("MUSIC_U");
        if (!cookie.empty() && cookie != music_u_) setMusicU(cookie);
    }
    return code;
}

bool NetEaseApi::dailySongs(std::vector<Song> &out) {
    auto j = call("v3/discovery/recommend/songs", nullptr);
    if (!j.contains("data") || !j["data"].is_object()) return false;
    if (!j["data"].contains("dailySongs") || !j["data"]["dailySongs"].is_array()) return false;
    out.clear();
    for (const auto &item : j["data"]["dailySongs"]) out.push_back(parseSong(item));
    return !out.empty();
}

bool NetEaseApi::recentSongs(std::vector<Song> &out) {
    auto j = call("v1/play/record", {{"uid", std::to_string(uid_)}, {"type", "1"}});
    const nlohmann::json *items = nullptr;
    if (j.contains("allData") && j["allData"].is_array()) items = &j["allData"];
    else if (j.contains("weekData") && j["weekData"].is_array()) items = &j["weekData"];
    if (!items) return false;

    out.clear();
    for (const auto &item : *items) {
        if (!item.contains("song") || !item["song"].is_object()) continue;
        out.push_back(parseSong(item["song"]));
    }
    return !out.empty();
}

bool NetEaseApi::userPlaylists(std::vector<Playlist> &out) {
    auto j = call("user/playlist",
                  {{"uid", std::to_string(uid_)},
                   {"limit", "1000"},
                   {"offset", "0"},
                   {"includeVideo", "true"}});
    if (!j.contains("playlist") || !j["playlist"].is_array()) return false;
    out.clear();
    for (const auto &item : j["playlist"]) {
        Playlist p;
        p.id = (uint64_t)jsonInt(item, "id");
        p.name = jsonStr(item, "name");
        p.track_count = (int)jsonInt(item, "trackCount");
        p.special_type = (int)jsonInt(item, "specialType");
        p.subscribed = jsonBool(item, "subscribed");
        p.cover_url = jsonStr(item, "coverImgUrl");
        if (item.contains("creator") && item["creator"].is_object()) {
            p.creator = jsonStr(item["creator"], "nickname");
        }
        if (p.id != 0) out.push_back(p);
    }
    return !out.empty();
}

bool NetEaseApi::collectedPlaylists(std::vector<Playlist> &out) {
    auto j = call("user/playlist/collect",
                  {{"userId", std::to_string(uid_)}, {"limit", "1000"}});
    const nlohmann::json *items = nullptr;
    if (j.contains("data") && j["data"].is_object() &&
        j["data"].contains("playlist") && j["data"]["playlist"].is_array()) {
        items = &j["data"]["playlist"];
    }
    if (!items) return false;

    out.clear();
    for (const auto &item : *items) {
        Playlist p;
        p.id = (uint64_t)jsonInt(item, "id");
        p.name = jsonStr(item, "name");
        p.track_count = (int)jsonInt(item, "trackCount");
        p.special_type = (int)jsonInt(item, "specialType");
        p.subscribed = true;
        p.cover_url = jsonStr(item, "coverImgUrl");
        if (item.contains("creator") && item["creator"].is_object()) {
            p.creator = jsonStr(item["creator"], "nickname");
        }
        if (p.id != 0) out.push_back(p);
    }
    return !out.empty();
}

bool NetEaseApi::profileBackground(std::string &url) {
    auto j = call("user/page/pretend/info", {{"userId", std::to_string(uid_)}});
    if (!j.contains("data") || !j["data"].is_object()) return false;
    const auto &d = j["data"];
    if (!d.contains("backgroundValue") || !d["backgroundValue"].is_object()) return false;
    const auto &bg = d["backgroundValue"];
    url = jsonStr(bg, "fullPicValue");
    if (url.empty()) url = jsonStr(bg, "singlePicValue");
    return !url.empty();
}

bool NetEaseApi::toplists(std::vector<Playlist> &out) {
    auto j = call("toplist/detail", nullptr);
    if (!j.contains("list") || !j["list"].is_array()) return false;
    out.clear();
    for (const auto &item : j["list"]) {
        Playlist p;
        p.id = (uint64_t)jsonInt(item, "id");
        p.name = jsonStr(item, "name");
        p.track_count = (int)jsonInt(item, "trackCount");
        p.creator = jsonStr(item, "updateFrequency");
        p.cover_url = jsonStr(item, "coverImgUrl");
        if (p.id != 0) out.push_back(p);
    }
    return !out.empty();
}

bool NetEaseApi::recommendPlaylists(std::vector<Playlist> &out) {
    auto j = call("personalized/playlist",
                  {{"limit", "30"}, {"total", "true"}, {"n", "1000"}});
    const nlohmann::json *items = nullptr;
    if (j.contains("result") && j["result"].is_array()) items = &j["result"];
    else if (j.contains("playlists") && j["playlists"].is_array()) items = &j["playlists"];
    if (!items) return false;
    out.clear();
    for (const auto &item : *items) {
        Playlist p;
        p.id = (uint64_t)jsonInt(item, "id");
        p.name = jsonStr(item, "name");
        p.track_count = (int)jsonInt(item, "trackCount");
        p.creator = jsonStr(item, "copywriter");
        p.cover_url = jsonStr(item, "picUrl");
        if (p.id != 0) out.push_back(p);
    }
    return !out.empty();
}

bool NetEaseApi::playlistDetail(uint64_t id, std::vector<Song> &out, std::string &name) {
    // v4 requires a cache_key: Base64(AES-ECB(")(13daqP@ssw0rd~", sorted params)).
    std::string sid = std::to_string(id);
    std::string cache_key = crypto::cacheKey({{"id", sid}, {"n", "1000"}, {"s", "8"}});

    auto j = call("playlist/v4/detail",
                  {{"id", sid},
                   {"n", "1000"},
                   {"s", "8"},
                   {"cache_key", cache_key}});

    if (j.contains("playlist") && j["playlist"].is_object() &&
        j["playlist"].contains("tracks") && j["playlist"]["tracks"].is_array() &&
        !j["playlist"]["tracks"].empty()) {
        const auto &pl = j["playlist"];
        name = jsonStr(pl, "name");
        out.clear();
        for (const auto &item : pl["tracks"]) out.push_back(parseSong(item));
        return !out.empty();
    }

    // If the cache_key is ever rejected or the playlist comes back empty, fall
    // back to v3, which takes a plain id.
    logf("[api] v4 playlist %llu unusable, falling back to v3",
         (unsigned long long)id);
    auto fallback = call("v3/playlist/detail",
                         {{"id", sid}, {"n", "1000"}, {"s", "8"}});
    if (!fallback.contains("playlist") || !fallback["playlist"].is_object()) return false;
    const auto &pl = fallback["playlist"];
    if (name.empty()) name = jsonStr(pl, "name");
    out.clear();
    if (pl.contains("tracks") && pl["tracks"].is_array()) {
        for (const auto &item : pl["tracks"]) out.push_back(parseSong(item));
    }
    return !out.empty();
}

bool NetEaseApi::songUrl(uint64_t id, int br, std::string &url, std::string &error) {
    const char *level = br >= 999000 ? "lossless"
                        : br >= 320000 ? "exhigh"
                        : br >= 192000 ? "higher"
                                       : "standard";
    nlohmann::json ids = nlohmann::json::array();
    ids.push_back(std::to_string(id) + "_0");

    auto j = call("song/enhance/player/url/v1", nlohmann::json(),
                  {{"ids", ids.dump()},
                   {"level", level},
                   {"encodeType", br >= 999000 ? "flac" : "mp3"},
                   {"trialMode", "0"}});

    if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
        const auto &first = j["data"][0];
        url = jsonStr(first, "url");
        // Log what the server actually handed back; cloud uploads often come
        // back as a different container than the level we asked for.
        logf("[api] url id=%llu code=%d fee=%d br=%d type=%s level=%s size=%lld url=%d",
             (unsigned long long)id, (int)jsonInt(first, "code"), (int)jsonInt(first, "fee"),
             (int)jsonInt(first, "br"), jsonStr(first, "type").c_str(),
             jsonStr(first, "level").c_str(),
             (long long)jsonInt(first, "size"), (int)!url.empty());
        if (!url.empty()) return true;
        if ((int)jsonInt(first, "code") == 404 || (int)jsonInt(first, "fee") != 0) {
            error = "该歌曲暂无版权或需要 VIP";
            return false;
        }
    }
    error = "获取播放地址失败";
    return false;
}

bool NetEaseApi::lyric(uint64_t id, std::string &lrc, std::string &translated) {
    auto j = call("song/lyric",
                  {{"id", std::to_string(id)},
                   {"lv", "-1"},
                   {"kv", "-1"},
                   {"tv", "-1"},
                   {"rv", "-1"}});
    lrc.clear();
    translated.clear();
    if (j.contains("lrc") && j["lrc"].is_object()) {
        lrc = jsonStr(j["lrc"], "lyric");
    }
    if (j.contains("tlyric") && j["tlyric"].is_object()) {
        translated = jsonStr(j["tlyric"], "lyric");
    }
    return !lrc.empty();
}

bool NetEaseApi::search(const std::string &keyword, std::vector<Song> &out) {
    auto j = call("search/tv/song/get", nlohmann::json(),
                  {{"keyword", keyword}, {"scene", "normal"}, {"trialMode", "0"},
                   {"limit", "20"}});
    out.clear();

    if (j.contains("data") && j["data"].is_object() && j["data"].contains("resources") &&
        j["data"]["resources"].is_array()) {
        for (const auto &item : j["data"]["resources"]) {
            if (!item.is_object()) continue;
            std::string type = jsonStr(item, "resourceType");
            if (type != "song") continue;
            if (!item.contains("baseInfo") || !item["baseInfo"].is_object()) continue;
            Song song = parseSong(item["baseInfo"]);
            if (song.id == 0) {
                song.id = (uint64_t)jsonInt(item, "resourceId");
            }
            if (song.id != 0) out.push_back(song);
        }
    }

    if (out.empty()) {
        auto fb = call("cloudsearch/pc", nlohmann::json(),
                       {{"s", keyword}, {"type", "1"}, {"limit", "50"}, {"offset", "0"}});
        if (fb.contains("result") && fb["result"].is_object() &&
            fb["result"].contains("songs") && fb["result"]["songs"].is_array()) {
            for (const auto &item : fb["result"]["songs"]) out.push_back(parseSong(item));
        }
    }
    return !out.empty();
}

bool NetEaseApi::likeSong(uint64_t id, bool like) {
    auto j = call("song/like",
                  {{"trackId", std::to_string(id)}, {"like", like ? "true" : "false"}});
    return (int)jsonInt(j, "code") == 200;
}

bool NetEaseApi::albumSongs(uint64_t album_id, std::vector<Song> &out, std::string &name,
                           std::string &cover) {
    auto j = call("v1/album/" + std::to_string(album_id), nlohmann::json());
    out.clear();
    name.clear();
    cover.clear();
    if (!j.contains("album") || !j["album"].is_object()) return false;

    const auto &al = j["album"];
    name = jsonStr(al, "name");
    cover = jsonStr(al, "picUrl");

    if (j.contains("songs") && j["songs"].is_array()) {
        for (const auto &item : j["songs"]) {
            Song s = parseSong(item);
            if (s.album.empty()) s.album = name;
            if (s.cover_url.empty()) s.cover_url = cover;
            out.push_back(s);
        }
    }
    return !out.empty();
}

bool NetEaseApi::artistHotSongs(uint64_t artist_id, std::vector<Song> &out, std::string &name) {
    auto j = call("v1/artist/" + std::to_string(artist_id), nlohmann::json());
    out.clear();
    name.clear();
    if (j.contains("artist") && j["artist"].is_object()) {
        name = jsonStr(j["artist"], "name");
    }
    if (j.contains("hotSongs") && j["hotSongs"].is_array()) {
        for (const auto &item : j["hotSongs"]) out.push_back(parseSong(item));
    }
    return !out.empty();
}

bool NetEaseApi::playlistSquare(std::vector<Playlist> &out) {
    // Toplists double as a browsable "square": 65 curated playlists.
    auto j = call("toplist", nlohmann::json());
    if (!j.contains("list") || !j["list"].is_array()) return false;
    out.clear();
    for (const auto &item : j["list"]) {
        Playlist p;
        p.id = (uint64_t)jsonInt(item, "id");
        p.name = jsonStr(item, "name");
        p.track_count = (int)jsonInt(item, "trackCount");
        p.cover_url = jsonStr(item, "coverImgUrl");
        p.creator = jsonStr(item, "updateFrequency");
        if (p.id != 0) out.push_back(p);
    }
    return !out.empty();
}

bool NetEaseApi::homeBlocks(std::vector<HomeBlock> &out) {
    auto j = call("music/multi/terminal/tv/homepage/get", nlohmann::json(),
                  {{"cursor", "0"},
                   {"next", ""},
                   {"positionCode", "IOT_TV_HOME_RECOMMEND_PAGE"},
                   {"sendVipScene", ""}});
    if (!j.contains("data") || !j["data"].is_object()) return false;
    if (!j["data"].contains("blocks") || !j["data"]["blocks"].is_array()) return false;

    out.clear();
    for (const auto &item : j["data"]["blocks"]) {
        if (!item.is_object()) continue;

        HomeBlock b;
        b.showType = jsonStr(item, "showType");
        b.resourceType = jsonStr(item, "resourceType");
        b.title = jsonStr(item, "title");
        b.subtitle = jsonStr(item, "subTitle");
        b.track_count = (int)jsonInt(item, "playCount");

        std::string rid = jsonStr(item, "resourceId");
        if (!rid.empty()) b.id = strtoull(rid.c_str(), nullptr, 10);

        auto img_it = item.find("imageInfo");
        if (img_it != item.end() && img_it->is_object()) {
            const auto &img = *img_it;
            b.cover = jsonStr(img, "url");
            if (b.cover.empty()) b.cover = jsonStr(img, "onImageIconUrl");
            if (b.subtitle.empty()) b.subtitle = jsonStr(img, "onImageText");
        }

        if (b.title.empty()) continue;
        out.push_back(b);
    }
    return !out.empty();
}

} // namespace ntv
