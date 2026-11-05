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
    std::string cover_url;
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
    explicit NetEaseApi(HttpClient &http);

    bool loggedIn() const;

    bool account(uint64_t &uid, std::string &nickname);

    bool qrCreate(std::string &unikey);
    int qrCheck(const std::string &unikey);

    bool dailySongs(std::vector<Song> &out);
    bool recentSongs(std::vector<Song> &out);
    bool userPlaylists(std::vector<Playlist> &out);
    bool toplists(std::vector<Playlist> &out);
    bool recommendPlaylists(std::vector<Playlist> &out);
    bool playlistDetail(uint64_t id, std::vector<Song> &out, std::string &name);
    bool songUrl(uint64_t id, int br, std::string &url, std::string &error);
    bool lyric(uint64_t id, std::string &lrc, std::string &translated);
    bool search(const std::string &keyword, std::vector<Song> &out);
    bool likeSong(uint64_t id, bool like);

    std::string deviceId() const { return device_id_; }
    void setMusicU(const std::string &value);
    void logout();
    std::string musicU() const { return music_u_; }

    static Song parseSong(const nlohmann::json &j);

private:
    nlohmann::json call(const std::string &path, const nlohmann::json &body,
                        const nlohmann::json &query = nlohmann::json());
    std::string headerJson() const;
    std::string cookieHeader() const;

    HttpClient &http_;
    std::string device_id_;
    std::string music_u_;
    uint64_t uid_ = 0;
};

} // namespace ntv
