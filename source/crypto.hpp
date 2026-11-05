#pragma once

#include <string>

#include "json.hpp"

namespace ntv::crypto {

struct WeapiParams {
    std::string params;
    std::string enc_sec_key;
};

// Encrypts a JSON body the way music.163.com's web client does.
WeapiParams weapiEncrypt(const nlohmann::json &body);

std::string randomBase62(size_t len);

} // namespace ntv::crypto
