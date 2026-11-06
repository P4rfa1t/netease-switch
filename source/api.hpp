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
    bool subscribed = false;
    std::string creator;
    std::string cover_url;
};

struct HomeCard {
    std::string showType;
    std::string resourceType;   // playlist / playList / dailySongs / song / album ...
    std::string title;
    std::string subtitle;
    std::string cover;
    std::string target;         // interactive.target (nmcloudmusictv://...)
    uint64_t id = 0;
    int track_count = 0;
};

struct Artist {
    uint64_t id = 0;
    std::string name;
    std::string avatar;
    int track_count = 0;
};

struct HomeBlock {
    std::string showType;       // GREETING / BIG_CARD / PLAYLIST / MUSIC / ...
    std::string title;
    std::vector<HomeCard> cards;
};

class NetEaseApi {
public:
    explicit NetEaseApi(HttpClient &http);

    bool loggedIn() const;

    bool account(uint64_t &uid, std::string &nickname,
                  std::string *avatar = nullptr, std::string *vip = nullptr);

    bool qrCreate(std::string &unikey);
    int qrCheck(const std::string &unikey);

    bool dailySongs(std::vector<Song> &out);
    bool recentSongs(std::vector<Song> &out);
    bool userPlaylists(std::vector<Playlist> &out);
    bool collectedPlaylists(std::vector<Playlist> &out);
    bool profileBackground(std::string &url);
    bool toplists(std::vector<Playlist> &out);
    bool recommendPlaylists(std::vector<Playlist> &out);
    bool playlistDetail(uint64_t id, std::vector<Song> &out, std::string &name);
    bool songUrl(uint64_t id, int br, std::string &url, std::string &error);
    bool lyric(uint64_t id, std::string &lrc, std::string &translated);
    bool search(const std::string &keyword, std::vector<Song> &out);
    bool likeSong(uint64_t id, bool like);

    // Extra browse pages
    bool albumSongs(uint64_t album_id, std::vector<Song> &out, std::string &name,
                    std::string &cover);
    bool artistHotSongs(uint64_t artist_id, std::vector<Song> &out, std::string &name);
    bool playlistSquare(std::vector<Playlist> &out);
    bool homeBlocks(std::vector<HomeBlock> &out);
    bool followedArtists(std::vector<Artist> &out, std::string &error);
    bool collectedAlbums(std::vector<Playlist> &out, std::string &error);

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
