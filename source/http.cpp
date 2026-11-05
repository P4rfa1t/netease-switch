#include "http.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include <curl/curl.h>
#include <switch.h>

#include "json.hpp"
#include "util.hpp"

namespace ntv {

namespace {
const char *kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36";
const char *kCaPath = "romfs:/cacert.pem";

std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
} // namespace

HttpClient::HttpClient() {
    curl_ = curl_easy_init();
    struct stat st{};
    has_ca_ = stat(kCaPath, &st) == 0;
    if (!has_ca_) logf("[http] warning: %s missing, TLS verification disabled", kCaPath);
}

HttpClient::~HttpClient() {
    if (curl_) curl_easy_cleanup(curl_);
}

void HttpClient::prepare(const std::string &url) {
    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl_, CURLOPT_REFERER, "https://music.163.com/");
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);

    if (has_ca_) {
        curl_easy_setopt(curl_, CURLOPT_CAINFO, kCaPath);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}

size_t HttpClient::writeToString(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    static_cast<std::string *>(userdata)->append(ptr, total);
    return total;
}

size_t HttpClient::writeToVector(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    auto *out = static_cast<std::vector<unsigned char> *>(userdata);
    out->insert(out->end(), (unsigned char *)ptr, (unsigned char *)ptr + total);
    return total;
}

size_t HttpClient::headerCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    static_cast<HttpClient *>(userdata)->onHeaderLine(std::string(buffer, total));
    return total;
}

void HttpClient::onHeaderLine(const std::string &line) {
    std::string l = lower(line);
    if (l.rfind("set-cookie:", 0) != 0) return;

    std::string value = line.substr(11);
    while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(value.begin());
    size_t end = value.find(';');
    if (end != std::string::npos) value = value.substr(0, end);
    size_t eq = value.find('=');
    if (eq == std::string::npos || eq == 0) return;

    std::string name = value.substr(0, eq);
    std::string val = value.substr(eq + 1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.pop_back();

    if (lower(name) == "max-age" || lower(name) == "path" || lower(name) == "domain" ||
        lower(name) == "expires") {
        return;
    }
    cookies_[name] = val;
}

void HttpClient::applyCookies() {}

HttpResult HttpClient::get(const std::string &url) {
    HttpResult result;
    prepare(url);
    curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &HttpClient::writeToString);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &HttpClient::headerCallback);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, this);

    curl_slist *list = nullptr;
    std::string cookie = cookieHeader();
    if (!cookie.empty()) list = curl_slist_append(list, ("Cookie: " + cookie).c_str());
    if (list) curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, list);

    CURLcode code = curl_easy_perform(curl_);
    curl_slist_free_all(list);
    if (code != CURLE_OK) {
        logf("[http] GET %s failed: %s", url.c_str(), curl_easy_strerror(code));
        result.status = 0;
    } else {
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &result.status);
    }
    return result;
}

HttpResult HttpClient::postForm(const std::string &url, const std::string &body) {
    HttpResult result;
    prepare(url);
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &HttpClient::writeToString);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &HttpClient::headerCallback);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, this);

    curl_slist *list = curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded");
    list = curl_slist_append(list, "Accept: */*");
    list = curl_slist_append(list, "Origin: https://music.163.com");
    std::string cookie = cookieHeader();
    if (!cookie.empty()) list = curl_slist_append(list, ("Cookie: " + cookie).c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, list);

    CURLcode code = curl_easy_perform(curl_);
    curl_slist_free_all(list);

    if (code != CURLE_OK) {
        logf("[http] POST %s failed: %s", url.c_str(), curl_easy_strerror(code));
        result.status = 0;
    } else {
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &result.status);
    }
    return result;
}

int HttpClient::progressCallback(void *userdata, long long, long long, long long, long long) {
    auto *cancel = static_cast<const std::atomic<bool> *>(userdata);
    return (cancel && cancel->load()) ? 1 : 0;
}

bool HttpClient::download(const std::string &url, std::vector<unsigned char> &out,
                          const std::atomic<bool> *cancel) {
    prepare(url);
    curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &HttpClient::writeToVector);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &HttpClient::headerCallback);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 120L);
    if (cancel) {
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, &HttpClient::progressCallback);
        curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, cancel);
    }

    CURLcode code = curl_easy_perform(curl_);
    long status = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
    if (code != CURLE_OK || status < 200 || status >= 300) {
        logf("[http] download failed: code=%d status=%ld", (int)code, status);
        return false;
    }
    return !out.empty();
}

std::string HttpClient::cookie(const std::string &name) const {
    auto it = cookies_.find(name);
    return it == cookies_.end() ? std::string() : it->second;
}

void HttpClient::setCookie(const std::string &name, const std::string &value) {
    cookies_[name] = value;
}

std::string HttpClient::cookieHeader() const {
    std::string out;
    for (const auto &kv : cookies_) {
        if (kv.second.empty()) continue;
        if (!out.empty()) out += "; ";
        out += kv.first + "=" + kv.second;
    }
    return out;
}

void HttpClient::loadCookies(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;
    std::string data;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);

    try {
        auto j = nlohmann::json::parse(data);
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) cookies_[it.key()] = it.value().get<std::string>();
        }
        logf("[http] loaded %d cookies", (int)cookies_.size());
    } catch (const std::exception &e) {
        logf("[http] cookie parse failed: %s", e.what());
    }
}

void HttpClient::loadCookieText(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;
    std::string data;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);

    const char separators[] = {';', 10, 13, 0};
    size_t start = 0;
    while (start < data.size()) {
        size_t end = data.find_first_of(separators, start);
        if (end == std::string::npos) end = data.size();
        std::string part = data.substr(start, end - start);
        start = end + 1;

        while (!part.empty() && (part.front() == ' ' || part.front() == 9)) part.erase(part.begin());
        while (!part.empty() && (part.back() == ' ' || part.back() == 9)) part.pop_back();
        size_t eq = part.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        cookies_[part.substr(0, eq)] = part.substr(eq + 1);
    }
    logf("[http] loaded %d cookies from %s", (int)cookies_.size(), path.c_str());
}

void HttpClient::saveCookies(const std::string &path) const {
    nlohmann::json j = nlohmann::json::object();
    for (const auto &kv : cookies_) j[kv.first] = kv.second;
    std::string data = j.dump();
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

} // namespace ntv
