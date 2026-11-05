#pragma once

#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

namespace ntv::crypto {

// eapi 请求加密：inner 必须是 "/api/..."，返回大写 hex
std::string eapiEncrypt(const std::string &inner_path, const std::string &json);

// eapi 响应解密：服务端对部分接口返回 AES-128-ECB 密文
bool eapiDecrypt(const std::string &raw, std::string &out);

// NginxPc cache_key: Base64(AES-128-ECB(")(13daqP@ssw0rd~", sortedParams))
std::string cacheKey(const std::vector<std::pair<std::string, std::string>> &params);

std::string md5Hex(const std::string &data);
std::string randomHex(size_t len);

} // namespace ntv::crypto
