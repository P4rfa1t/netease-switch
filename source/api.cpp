#include "api.hpp"

#include <exception>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "util.hpp"

namespace ntv {

namespace {

std::string joinArtists(const nlohmann::json &song) {
    std::string out;
    if (song.contains("ar") && song["ar"].is_array()) {
        for (const auto &a : song["ar"]) {
            if (!out.empty()) out += " / ";
            out += a.value("name", "");
        }
    } else if (song.contains("artists") && song["artists"].is_array()) {
        for (const auto &a : song["artists"]) {
            if (!out.empty()) out += " / ";
            out += a.value("name", "");
        }
    }
    return out;
}

std::string albumName(const nlohmann::json &song) {
    if (song.contains("al") && song["al"].is_object()) return song["al"].value("name", "");
    if (song.contains("album") && song["album"].is_object()) return song["album"].value("name", "");
    return "";
}

} // namespace

std::string NetEaseApi::csrf() const {
    return http_.cookie("__csrf");
}

bool NetEaseApi::loggedIn() const {
    return !http_.cookie("MUSIC_U").empty();
}

nlohmann::json NetEaseApi::post(const std::string &path, const nlohmann::json &body) {
    auto params = crypto::weapiEncrypt(body);
    std::string url = "https://music.163.com" + path + "?csrf_token=" + urlEncode(csrf());
    std::string form =
        "params=" + urlEncode(params.params) + "&encSecKey=" + urlEncode(params.enc_sec_key);

    HttpResult r = http_.postForm(url, form);
    if (!r.ok()) {
        logf("[api] POST %s http=%ld", path.c_str(), r.status);
        return nlohmann::json::object();
    }
    try {
        auto j = nlohmann::json::parse(r.body);
        if (j.contains("code") && j["code"].is_number() && j["code"].get<int>() != 200) {
            logf("[api] POST %s code=%d", path.c_str(), j["code"].get<int>());
        }
        return j;
    } catch (const std::exception &e) {
        logf("[api] POST %s parse error: %s", path.c_str(), e.what());
        return nlohmann::json::object();
    }
}

Song NetEaseApi::parseSong(const nlohmann::json &j) {
    Song s;
    s.id = j.value("id", (uint64_t)0);
    s.name = j.value("name", "");
    s.artist = joinArtists(j);
    s.album = albumName(j);
    s.duration_ms = j.value("dt", 0);
    return s;
}

bool NetEaseApi::account(uint64_t &uid, std::string &nickname) {
    auto j = post("/weapi/w/nuser/account/get",
                  {{"csrf_token", csrf()}, {"needExp", true}});
    if (!j.contains("account") || j["account"].is_null()) return false;
    uid = j["account"].value("id", (uint64_t)0);
    nickname = j.contains("profile") && j["profile"].is_object()
                   ? j["profile"].value("nickname", "")
                   : "";
    return uid != 0;
}

bool NetEaseApi::qrCreate(std::string &unikey) {
    auto j = post("/weapi/login/qrcode/unikey", {{"type", 1}, {"csrf_token", csrf()}});
    if (j.value("code", 0) != 200) return false;
    unikey = j.value("unikey", "");
    return !unikey.empty();
}

int NetEaseApi::qrCheck(const std::string &unikey) {
    auto j = post("/weapi/login/qrcode/client/login",
                  {{"key", unikey}, {"type", 1}, {"csrf_token", csrf()}});
    return j.value("code", 0);
}

bool NetEaseApi::dailySongs(std::vector<Song> &out) {
    auto j = post("/weapi/v3/discovery/recommend/songs", {{"csrf_token", csrf()}});
    if (!j.contains("data") || !j["data"].is_object()) return false;
    if (!j["data"].contains("dailySongs") || !j["data"]["dailySongs"].is_array()) return false;
    out.clear();
    for (const auto &item : j["data"]["dailySongs"]) out.push_back(parseSong(item));
    return !out.empty();
}

bool NetEaseApi::recentSongs(uint64_t uid, std::vector<Song> &out) {
    auto j = post("/weapi/v1/play/record", {{"uid", uid}, {"type", 1}, {"csrf_token", csrf()}});
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

bool NetEaseApi::userPlaylists(uint64_t uid, std::vector<Playlist> &out) {
    auto j = post("/weapi/user/playlist",
                  {{"uid", uid},
                   {"limit", 1000},
                   {"offset", 0},
                   {"includeVideo", true},
                   {"csrf_token", csrf()}});
    if (!j.contains("playlist") || !j["playlist"].is_array()) return false;
    out.clear();
    for (const auto &item : j["playlist"]) {
        Playlist p;
        p.id = item.value("id", (uint64_t)0);
        p.name = item.value("name", "");
        p.track_count = item.value("trackCount", 0);
        if (item.contains("creator") && item["creator"].is_object()) {
            p.creator = item["creator"].value("nickname", "");
        }
        if (p.id != 0) out.push_back(p);
    }
    return !out.empty();
}

bool NetEaseApi::playlistDetail(uint64_t id, std::vector<Song> &out, std::string &name) {
    auto j = post("/weapi/v6/playlist/detail",
                  {{"id", id}, {"n", 1000}, {"s", 8}, {"csrf_token", csrf()}});
    if (!j.contains("playlist") || !j["playlist"].is_object()) return false;
    const auto &pl = j["playlist"];
    name = pl.value("name", "");
    out.clear();

    if (pl.contains("tracks") && pl["tracks"].is_array()) {
        for (const auto &item : pl["tracks"]) out.push_back(parseSong(item));
    }

    // Playlists longer than 1000 tracks only return the first page inline.
    int total = pl.value("trackCount", (int)out.size());
    while ((int)out.size() < total && !out.empty()) {
        auto page = post("/weapi/v6/playlist/track/all",
                         {{"id", id},
                          {"offset", (int)out.size()},
                          {"limit", 1000},
                          {"n", 1000},
                          {"csrf_token", csrf()}});
        if (!page.contains("songs") || !page["songs"].is_array() || page["songs"].empty()) break;
        for (const auto &item : page["songs"]) out.push_back(parseSong(item));
        if (page["songs"].size() < 1000) break;
    }
    return !out.empty();
}

bool NetEaseApi::songUrl(uint64_t id, std::string &url, std::string &error) {
    std::string ids = "[" + std::to_string(id) + "]";
    auto j = post("/weapi/song/enhance/player/url",
                  {{"ids", ids}, {"br", 320000}, {"csrf_token", csrf()}});
    if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
        url = j["data"][0].value("url", "");
        if (!url.empty()) return true;
    }
    error = "该歌曲暂无版权或需要 VIP";
    return false;
}

bool NetEaseApi::lyric(uint64_t id, std::string &lrc) {
    auto j = post("/weapi/song/lyric",
                  {{"id", id}, {"lv", -1}, {"kv", -1}, {"tv", -1}, {"csrf_token", csrf()}});
    if (!j.contains("lrc") || !j["lrc"].is_object()) return false;
    lrc = j["lrc"].value("lyric", "");
    return !lrc.empty();
}

bool NetEaseApi::search(const std::string &keyword, std::vector<Song> &out) {
    auto j = post("/weapi/cloudsearch/pc",
                  {{"s", keyword},
                   {"type", 1},
                   {"limit", 50},
                   {"offset", 0},
                   {"total", true},
                   {"csrf_token", csrf()}});
    if (!j.contains("result") || !j["result"].is_object()) return false;
    if (!j["result"].contains("songs") || !j["result"]["songs"].is_array()) return false;
    out.clear();
    for (const auto &item : j["result"]["songs"]) out.push_back(parseSong(item));
    return !out.empty();
}

} // namespace ntv
