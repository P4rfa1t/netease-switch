#include "util.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace ntv {

static FILE *g_log = nullptr;

void logInit() {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/netease-switch", 0777);
    g_log = fopen("sdmc:/switch/netease-switch/last.log", "w");
}

void logf(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("%s\n", buf);
    if (g_log) {
        fprintf(g_log, "%s\n", buf);
        fflush(g_log);
    }
}

std::string urlEncode(const std::string &in) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string formatTime(double seconds) {
    if (seconds < 0) seconds = 0;
    int total = (int)seconds;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}

std::string formatDuration(int ms) {
    if (ms < 0) ms = 0;
    return formatTime(ms / 1000.0);
}

size_t utf8Length(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

std::string utf8Truncate(const std::string &s, size_t maxChars) {
    size_t chars = 0;
    size_t i = 0;
    while (i < s.size() && chars < maxChars) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) break;
        i += len;
        ++chars;
    }
    if (i < s.size()) return s.substr(0, i) + "…";
    return s;
}

} // namespace ntv
