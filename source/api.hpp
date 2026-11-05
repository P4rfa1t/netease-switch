#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "http.hpp"
#include "json.hpp"

namespace ntv {

struct Song {
    uint64_t id = 0;
    std::string name;
    std::string artist;
    std::string album;
    int duration_ms = 0;
};

struct Playlist {
    uint64_t id = 0;
    std::string name;
    int track_count = 0;
    int special_type = 0;
    std::string creator;
};

class NetEaseApi {
public:
    explicit NetEaseApi(HttpClient &http) : http_(http) {
        // The web login flow expects a desktop client fingerprint.
        if (http_.cookie("os").empty()) http_.setCookie("os", "pc");
        if (http_.cookie("appver").empty()) http_.setCookie("appver", "2.7.1.198277");
        if (http_.cookie("__remember_me").empty()) http_.setCookie("__remember_me", "true");
    }

    std::string csrf() const;
    bool loggedIn() const;

    bool account(uint64_t &uid, std::string &nickname);

    bool qrCreate(std::string &unikey);
    int qrCheck(const std::string &unikey);

    bool dailySongs(std::vector<Song> &out);
    bool recentSongs(uint64_t uid, std::vector<Song> &out);
    bool userPlaylists(uint64_t uid, std::vector<Playlist> &out);
    bool toplists(std::vector<Playlist> &out);
    bool playlistDetail(uint64_t id, std::vector<Song> &out, std::string &name);
    bool songUrl(uint64_t id, std::string &url, std::string &error);
    bool lyric(uint64_t id, std::string &lrc);
    bool likeSong(uint64_t id, bool like);
    bool search(const std::string &keyword, std::vector<Song> &out);

    static Song parseSong(const nlohmann::json &j);

private:
    nlohmann::json post(const std::string &path, const nlohmann::json &body);

    HttpClient &http_;
};

} // namespace ntv
