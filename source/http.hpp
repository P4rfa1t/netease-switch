#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

typedef void CURL;

namespace ntv {

struct HttpResult {
    long status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;

    HttpResult get(const std::string &url);
    HttpResult postForm(const std::string &url, const std::string &body);
    bool download(const std::string &url, std::vector<unsigned char> &out,
                  const std::atomic<bool> *cancel = nullptr);

    std::string cookie(const std::string &name) const;
    void setCookie(const std::string &name, const std::string &value);
    std::string cookieHeader() const;

    void loadCookies(const std::string &path);
    void loadCookieText(const std::string &path);
    void saveCookies(const std::string &path) const;

private:
    CURL *curl_ = nullptr;
    std::map<std::string, std::string> cookies_;
    bool has_ca_ = false;

    void prepare(const std::string &url);
    void onHeaderLine(const std::string &line);
    void applyCookies();

    static size_t writeToString(char *ptr, size_t size, size_t nmemb, void *userdata);
    static size_t writeToVector(char *ptr, size_t size, size_t nmemb, void *userdata);
    static size_t headerCallback(char *buffer, size_t size, size_t nitems, void *userdata);
    static int progressCallback(void *userdata, long long dltotal, long long dlnow,
                                long long ultotal, long long ulnow);
};

} // namespace ntv
