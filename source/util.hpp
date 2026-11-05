#pragma once

#include <cstddef>
#include <string>

namespace ntv {

double clockSeconds();
void logInit();
void logf(const char *fmt, ...);

std::string urlEncode(const std::string &in);
std::string formatTime(double seconds);
std::string formatDuration(int ms);

size_t utf8Length(const std::string &s);
std::string utf8Truncate(const std::string &s, size_t maxChars);

} // namespace ntv
