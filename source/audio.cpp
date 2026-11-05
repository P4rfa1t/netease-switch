#include "audio.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <SDL2/SDL.h>

#include "dr_flac.h"
#include "ffmpeg_decoder.hpp"
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

    // stop() only resets the flag when it actually joined a thread, so clear it
    // here or the fresh thread sees stop_==true and exits immediately.
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
    stop_ = false;
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

    // Sniff the container instead of trusting the requested format. Note that
    // most streams start with an ID3v2 tag, so the real marker sits after it.
    size_t probe = 0;
    bool has_id3 = file.size() > 10 && memcmp(file.data(), "ID3", 3) == 0;
    if (has_id3) {
        // ID3v2 size is a 28-bit syncsafe integer at offset 6.
        size_t tag = ((size_t)(file[6] & 0x7F) << 21) | ((size_t)(file[7] & 0x7F) << 14) |
                     ((size_t)(file[8] & 0x7F) << 7) | (size_t)(file[9] & 0x7F);
        probe = 10 + tag;
        if (probe + 8 > file.size()) probe = 0;
    }

    const unsigned char *body = file.data() + probe;
    size_t body_len = file.size() - probe;

    bool is_flac = body_len > 4 && memcmp(body, "fLaC", 4) == 0;
    bool is_m4a = body_len > 12 && memcmp(body + 4, "ftyp", 4) == 0;
    bool is_aac = body_len > 2 && body[0] == 0xFF && (body[1] & 0xF6) == 0xF0;
    bool is_mp3 = body_len > 2 && body[0] == 0xFF && (body[1] & 0xE0) == 0xE0;
    bool looks_html = body_len > 5 && memcmp(body, "<!DOC", 5) == 0;
    bool looks_json = body_len > 1 && body[0] == '{';

    char magic[8] = {};
    for (int i = 0; i < 4 && (size_t)i < body_len; ++i) {
        magic[i] = (body[i] >= 32 && body[i] < 127) ? (char)body[i] : '.';
    }
    logf("[audio] %zu bytes, id3=%d probe=%zu magic=%02X%02X%02X%02X (%s)", file.size(),
         (int)has_id3, probe, body_len > 0 ? body[0] : 0, body_len > 1 ? body[1] : 0,
         body_len > 2 ? body[2] : 0, body_len > 3 ? body[3] : 0, magic);

    drmp3 mp3{};
    drflac *flac = nullptr;
    FfmpegDecoder *ffd = nullptr;
    bool using_ffmpeg = false;
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
    } else if (is_m4a || (is_aac && !is_mp3)) {
        // Cloud uploads arrive as M4A/AAC; hand those to libavformat.
        using_ffmpeg = true;
        ffd = new FfmpegDecoder();
        if (!ffd->open(file, "m4a")) {
            delete ffd;
            ffd = nullptr;
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = "M4A/AAC 解码失败";
            state_ = State::Error;
            return;
        }
        channels = ffd->channels();
        rate = (uint32_t)ffd->sampleRate();
        total = ffd->totalFrames();
    } else {
        // ID3-tagged streams are still plain MP3 frames inside.
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
        if (ffd) delete ffd;
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
    uint64_t frames_produced = 0;
    while (!stop_) {
        int64_t request = seek_request_.exchange(-1);
        if (request >= 0) {
            uint64_t target = (uint64_t)request;
            if (total > 0 && target > total) target = total;
            if (using_ffmpeg) ffd->seek((double)target / (double)rate);
            else if (is_flac) drflac_seek_to_pcm_frame(flac, target);
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

        uint64_t got = using_ffmpeg
                           ? (uint64_t)ffd->read(chunk.data(), (int)kChunkFrames)
                       : is_flac ? drflac_read_pcm_frames_s16(flac, kChunkFrames, chunk.data())
                               : drmp3_read_pcm_frames_s16(&mp3, kChunkFrames, chunk.data());
        if (got == 0) break;
        frames_produced += got;
        appendStereo(chunk.data(), (size_t)got, channels);
    }

    if (frames_produced == 0 && !stop_) {
        // The decoder opened but never produced audio; treat it as an error so
        // the UI reports the problem instead of auto-skipping the track.
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "解码后没有得到音频数据";
        state_ = State::Error;
        SDL_CloseAudioDevice(device);
        device_id_ = 0;
        if (flac) drflac_close(flac);
        if (ffd) delete ffd;
        drmp3_uninit(&mp3);
        return;
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
    if (ffd) delete ffd;
    drmp3_uninit(&mp3);
}

} // namespace ntv
