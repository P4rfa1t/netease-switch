#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <switch.h>

namespace ntv {

class Player {
public:
    enum class State { Idle, Loading, Playing, Paused, Finished, Error };

    Player() = default;
    ~Player();

    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    bool init();
    void shutdown();

    void play(const std::string &url, const std::string &title, const std::string &artist);
    void togglePause();
    void stop();
    void seek(double seconds);

    void adjustVolume(float delta);
    float volume() const { return volume_.load(); }

    State state() const { return state_.load(); }
    bool finished() const { return state_.load() == State::Finished; }
    double position() const;
    double duration() const;

    std::string title() const;
    std::string artist() const;
    std::string error() const;

private:
    void threadMain();
    void audioCallback(uint8_t *stream, int len);
    static void sdlCallback(void *userdata, uint8_t *stream, int len);
    static void threadEntry(void *arg);

    size_t ringFree() const;
    void appendStereo(const int16_t *interleaved, size_t frames, int channels);

    Thread thread_{};
    bool thread_running_ = false;
    std::atomic<bool> stop_{false};

    mutable std::mutex mutex_;
    std::vector<int16_t> ring_;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
    size_t count_ = 0;

    std::atomic<State> state_{State::Idle};
    std::atomic<float> volume_{0.7f};
    std::atomic<bool> paused_{false};

    uint32_t device_id_ = 0;
    double sample_rate_ = 44100.0;
    uint64_t total_frames_ = 0;
    uint64_t played_frames_ = 0;
    uint64_t seek_offset_frames_ = 0;
    std::atomic<int64_t> seek_request_{-1};

    std::string title_;
    std::string artist_;
    std::string error_;
    std::string pending_url_;
};

} // namespace ntv
