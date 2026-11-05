#include <curl/curl.h>

#include "app.hpp"

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    ntv::App app;
    int result = app.run();
    curl_global_cleanup();
    return result;
}
