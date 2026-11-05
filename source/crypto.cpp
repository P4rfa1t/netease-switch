#include "crypto.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <switch.h>

namespace ntv::crypto {

namespace {

const char *kEapiKey = "e82ckenh8dichen8";
const char *kSplit = "-36cd479b6b5-";

std::vector<unsigned char> aesEcb(const unsigned char *key, const std::vector<unsigned char> &in,
                                  bool encrypt) {
    std::vector<unsigned char> buf = in;
    if (encrypt) {
        size_t pad = 16 - (buf.size() % 16);
        buf.insert(buf.end(), pad, (unsigned char)pad);
    } else if (buf.size() % 16 != 0) {
        return {};
    }

    std::vector<unsigned char> out(buf.size());
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (encrypt) {
        mbedtls_aes_setkey_enc(&ctx, key, 128);
        for (size_t off = 0; off < buf.size(); off += 16) {
            mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, buf.data() + off, out.data() + off);
        }
    } else {
        mbedtls_aes_setkey_dec(&ctx, key, 128);
        for (size_t off = 0; off < buf.size(); off += 16) {
            mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, buf.data() + off, out.data() + off);
        }
    }
    mbedtls_aes_free(&ctx);
    return out;
}

std::string hexUpper(const std::vector<unsigned char> &data) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char c : data) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

} // namespace

std::string md5Hex(const std::string &data) {
    unsigned char digest[16];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts_ret(&ctx);
    mbedtls_md5_update_ret(&ctx, (const unsigned char *)data.data(), data.size());
    mbedtls_md5_finish_ret(&ctx, digest);
    mbedtls_md5_free(&ctx);

    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char c : digest) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

std::string randomHex(size_t len) {
    std::vector<unsigned char> raw(len);
    randomGet(raw.data(), raw.size());
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned char c : raw) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

std::string eapiEncrypt(const std::string &inner_path, const std::string &json) {
    std::string digest = md5Hex(std::string("nobody") + inner_path + "use" + json + "md5forencrypt");
    std::string plain = inner_path + kSplit + json + kSplit + digest;

    std::vector<unsigned char> in(plain.begin(), plain.end());
    std::vector<unsigned char> cipher =
        aesEcb((const unsigned char *)kEapiKey, in, true);
    return hexUpper(cipher);
}

bool eapiDecrypt(const std::string &raw, std::string &out) {
    if (raw.empty() || raw.size() % 16 != 0) return false;
    if (raw[0] == '{' || raw[0] == '[') return false;

    std::vector<unsigned char> in(raw.begin(), raw.end());
    std::vector<unsigned char> plain = aesEcb((const unsigned char *)kEapiKey, in, false);
    if (plain.empty()) return false;

    unsigned char pad = plain.back();
    if (pad >= 1 && pad <= 16) plain.resize(plain.size() - pad);

    out.assign(plain.begin(), plain.end());
    return !out.empty() && (out[0] == '{' || out[0] == '[');
}

} // namespace ntv::crypto
