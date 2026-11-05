#include "audio.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <SDL2/SDL.h>

#include "dr_flac.h"
#include "dr_mp3.h"
#include "http.hpp"
#include "util.hpp"

namespace ntv {

namespace {
constexpr size_t kChunkFrames = 2048;
constexpr size_t kRingSeconds = 3;
} // namespace

Player::~Player() {
    stop();
}

bool Player::init() {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            logf("[audio] SDL audio init failed: %s", SDL_GetError());
            return false;
        }
    }
    return true;
}

void Player::shutdown() {
    stop();
}

void Player::play(const std::string &url, const std::string &title, const std::string &artist) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        title_ = title;
        artist_ = artist;
        error_.clear();
        pending_url_ = url;
    }

    stop_ = false;
    paused_ = false;
    seek_request_ = -1;
    state_ = State::Loading;

    Result rc = threadCreate(&thread_, &Player::threadEntry, this, nullptr, 1024 * 1024, 0x2C, -2);
    if (R_FAILED(rc)) {
        logf("[audio] threadCreate failed: 0x%x", rc);
        state_ = State::Error;
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "无法创建播放线程";
        return;
    }
    threadStart(&thread_);
    thread_running_ = true;
}

void Player::stop() {
    if (thread_running_) {
        stop_ = true;
        threadWaitForExit(&thread_);
        threadClose(&thread_);
        thread_running_ = false;
    }
    if (device_id_) {
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.clear();
        read_pos_ = write_pos_ = count_ = 0;
        played_frames_ = 0;
        seek_offset_frames_ = 0;
        total_frames_ = 0;
    }
    seek_request_ = -1;
    paused_ = false;
    if (state_.load() != State::Error) state_ = State::Idle;
}

void Player::togglePause() {
    State s = state_.load();
    if (s == State::Playing) {
        paused_ = true;
        if (device_id_) SDL_PauseAudioDevice(device_id_, 1);
        state_ = State::Paused;
    } else if (s == State::Paused) {
        paused_ = false;
        if (device_id_) SDL_PauseAudioDevice(device_id_, 0);
        state_ = State::Playing;
    }
}

void Player::seek(double seconds) {
    State s = state_.load();
    if (s != State::Playing && s != State::Paused) return;
    if (seconds < 0) seconds = 0;
    int64_t frame = (int64_t)(seconds * sample_rate_);
    if (total_frames_ > 0 && (uint64_t)frame > total_frames_) frame = (int64_t)total_frames_;
    seek_request_ = frame;
}

void Player::adjustVolume(float delta) {
    float v = volume_.load() + delta;
    volume_ = std::clamp(v, 0.0f, 1.0f);
}

double Player::position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sample_rate_ <= 0) return 0.0;
    return (double)(seek_offset_frames_ + played_frames_) / sample_rate_;
}

double Player::duration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sample_rate_ <= 0) return 0.0;
    return (double)total_frames_ / sample_rate_;
}

std::string Player::title() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return title_;
}

std::string Player::artist() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return artist_;
}

std::string Player::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void Player::threadEntry(void *arg) {
    static_cast<Player *>(arg)->threadMain();
}

void Player::sdlCallback(void *userdata, uint8_t *stream, int len) {
    static_cast<Player *>(userdata)->audioCallback(stream, len);
}

void Player::audioCallback(uint8_t *stream, int len) {
    int16_t *out = (int16_t *)stream;
    size_t frames = (size_t)len / 4; // stereo s16
    size_t written = 0;
    float vol = volume_.load();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ring_.empty()) {
            memset(stream, 0, len);
            return;
        }
        size_t available = count_ / 2;
        size_t n = std::min(frames, available);
        size_t mask = ring_.size();
        for (size_t i = 0; i < n * 2; ++i) {
            int32_t v = (int32_t)(ring_[(read_pos_ + i) % mask] * vol);
            out[i] = (int16_t)std::clamp(v, -32768, 32767);
        }
        read_pos_ = (read_pos_ + n * 2) % mask;
        count_ -= n * 2;
        played_frames_ += n;
        written = n;
    }

    if (written < frames) {
        memset(out + written * 2, 0, (frames - written) * 4);
    }
}

size_t Player::ringFree() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ring_.empty() ? 0 : ring_.size() - count_;
}

void Player::appendStereo(const int16_t *in, size_t frames, int channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.empty()) return;
    size_t cap = ring_.size();
    for (size_t i = 0; i < frames; ++i) {
        int16_t l = in[i * channels];
        int16_t r = channels >= 2 ? in[i * channels + 1] : l;
        ring_[write_pos_] = l;
        write_pos_ = (write_pos_ + 1) % cap;
        ring_[write_pos_] = r;
        write_pos_ = (write_pos_ + 1) % cap;
        count_ += 2;
    }
}

void Player::threadMain() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = pending_url_;
    }

    HttpClient http;
    std::vector<unsigned char> file;
    if (!http.download(url, file, &stop_)) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "音频下载失败，请检查网络";
        state_ = State::Error;
        return;
    }
    if (stop_) return;

    // Sniff the container instead of trusting the requested format: cloud-disk
    // uploads are often AAC/M4A or FLAC even when we asked for an mp3 stream.
    char magic[16] = {};
    memcpy(magic, file.data(), file.size() < sizeof(magic) ? file.size() : sizeof(magic));
    logf("[audio] %zu bytes, magic=%02X%02X%02X%02X (%c%c%c%c)", file.size(),
         (unsigned char)magic[0], (unsigned char)magic[1], (unsigned char)magic[2],
         (unsigned char)magic[3],
         (magic[0] >= 32 && magic[0] < 127) ? magic[0] : '.',
         (magic[1] >= 32 && magic[1] < 127) ? magic[1] : '.',
         (magic[2] >= 32 && magic[2] < 127) ? magic[2] : '.',
         (magic[3] >= 32 && magic[3] < 127) ? magic[3] : '.');

    bool is_flac = file.size() > 4 && memcmp(file.data(), "fLaC", 4) == 0;
    bool is_m4a = file.size() > 12 && memcmp(file.data() + 4, "ftyp", 4) == 0;
    bool looks_html = file.size() > 5 && memcmp(file.data(), "<!DOC", 5) == 0;
    bool looks_json = !file.empty() && file[0] == '{';

    drmp3 mp3{};
    drflac *flac = nullptr;
    int channels = 2;
    uint32_t rate = 44100;
    uint64_t total = 0;

    if (looks_html || looks_json) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "取到的不是音频（链接已失效或需要重新登录）";
        state_ = State::Error;
        return;
    }

    if (is_flac) {
        flac = drflac_open_memory(file.data(), file.size(), nullptr);
        if (!flac) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = "FLAC 解码失败";
            state_ = State::Error;
            return;
        }
        channels = flac->channels;
        rate = flac->sampleRate;
        total = flac->totalPCMFrameCount;
    } else if (is_m4a) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "该歌曲是 M4A/AAC 格式，当前解码器不支持";
        state_ = State::Error;
        return;
    } else {
        if (!drmp3_init_memory(&mp3, file.data(), file.size(), nullptr)) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = "MP3 解码失败（格式可能不受支持）";
            state_ = State::Error;
            return;
        }
        channels = mp3.channels;
        rate = mp3.sampleRate;
        total = drmp3_get_pcm_frame_count(&mp3);
    }

    SDL_AudioSpec want{};
    SDL_AudioSpec have{};
    want.freq = (int)rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &Player::sdlCallback;
    want.userdata = this;

    uint32_t device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!device) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::string("音频设备打开失败: ") + SDL_GetError();
        state_ = State::Error;
        if (flac) drflac_close(flac);
        drmp3_uninit(&mp3);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sample_rate_ = rate;
        total_frames_ = total;
        played_frames_ = 0;
        seek_offset_frames_ = 0;
        read_pos_ = write_pos_ = count_ = 0;
        ring_.assign((size_t)rate * 2 * kRingSeconds, 0);
        device_id_ = device;
    }

    SDL_PauseAudioDevice(device, 0);
    state_ = State::Playing;

    std::vector<int16_t> chunk(kChunkFrames * 8);
    while (!stop_) {
        int64_t request = seek_request_.exchange(-1);
        if (request >= 0) {
            uint64_t target = (uint64_t)request;
            if (total > 0 && target > total) target = total;
            if (is_flac) drflac_seek_to_pcm_frame(flac, target);
            else drmp3_seek_to_pcm_frame(&mp3, target);
            std::lock_guard<std::mutex> lock(mutex_);
            read_pos_ = write_pos_ = count_ = 0;
            seek_offset_frames_ = target;
            played_frames_ = 0;
        }

        if (ringFree() < kChunkFrames * 2) {
            SDL_Delay(5);
            continue;
        }

        uint64_t got = is_flac ? drflac_read_pcm_frames_s16(flac, kChunkFrames, chunk.data())
                               : drmp3_read_pcm_frames_s16(&mp3, kChunkFrames, chunk.data());
        if (got == 0) break;
        appendStereo(chunk.data(), (size_t)got, channels);
    }

    while (!stop_) {
        bool drained;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            drained = count_ == 0;
        }
        if (drained) break;
        SDL_Delay(10);
    }

    if (!stop_) {
        state_ = State::Finished;
        paused_ = false;
        SDL_PauseAudioDevice(device, 1);
    }

    if (flac) drflac_close(flac);
    drmp3_uninit(&mp3);
}

} // namespace ntv
