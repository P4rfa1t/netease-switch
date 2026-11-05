#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ntv {

// Decodes any container libavformat understands (M4A/AAC, OGG, WAV...). Used as
// the fallback for cloud-disk uploads, which are often not MP3.
class FfmpegDecoder {
public:
    FfmpegDecoder();
    ~FfmpegDecoder();

    FfmpegDecoder(const FfmpegDecoder &) = delete;
    FfmpegDecoder &operator=(const FfmpegDecoder &) = delete;

    bool open(const std::vector<unsigned char> &data, const std::string &ext_hint);
    bool seek(double seconds);
    int read(int16_t *out, int frames);

    int sampleRate() const { return rate_; }
    int channels() const { return channels_; }
    uint64_t totalFrames() const { return total_frames_; }

private:
    struct Impl;
    Impl *impl_ = nullptr;
    int rate_ = 44100;
    int channels_ = 2;
    uint64_t total_frames_ = 0;
};

} // namespace ntv
