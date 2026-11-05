#include "crypto.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <switch.h>

namespace ntv::crypto {

namespace {

const char *kIv = "0102030405060708";
const char *kPresetKey = "0CoJUm6Qyw8W8jud";
const char *kBase62 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
const char *kRsaModulus =
    "00e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e"
    "417629ec4ee341f56135fccf695280104e0312ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee2559"
    "32575cce10b424d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";
const char *kRsaExponent = "010001";

std::vector<unsigned char> aesEncrypt(const std::vector<unsigned char> &plain,
                                      const unsigned char *key, bool cbc) {
    std::vector<unsigned char> buf = plain;
    size_t pad = 16 - (buf.size() % 16);
    buf.insert(buf.end(), pad, (unsigned char)pad);

    std::vector<unsigned char> out(buf.size());
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);

    if (cbc) {
        unsigned char iv[16];
        memcpy(iv, kIv, 16);
        mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, buf.size(), iv, buf.data(), out.data());
    } else {
        for (size_t off = 0; off < buf.size(); off += 16) {
            mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, buf.data() + off, out.data() + off);
        }
    }

    mbedtls_aes_free(&ctx);
    return out;
}

std::string base64(const std::vector<unsigned char> &data) {
    size_t olen = 0;
    std::string out(((data.size() + 2) / 3) * 4 + 1, '\0');
    if (mbedtls_base64_encode((unsigned char *)out.data(), out.size(), &olen, data.data(),
                              data.size()) != 0) {
        return {};
    }
    out.resize(olen);
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

std::string aesCbcBase64(const std::string &plain, const std::string &key) {
    std::vector<unsigned char> in(plain.begin(), plain.end());
    return base64(aesEncrypt(in, (const unsigned char *)key.data(), true));
}

// The client reverses the secret key, treats it as a big-endian integer,
// raises it to the public exponent modulo the 1024-bit modulus, and hex-encodes
// the result left-padded to 128 bytes.
std::string rsaEncrypt(const std::string &text) {
    std::string reversed(text.rbegin(), text.rend());

    mbedtls_mpi m, e, n, r;
    mbedtls_mpi_init(&m);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&r);

    std::string enc;
    if (mbedtls_mpi_read_binary(&m, (const unsigned char *)reversed.data(), reversed.size()) == 0 &&
        mbedtls_mpi_read_string(&e, 16, kRsaExponent) == 0 &&
        mbedtls_mpi_read_string(&n, 16, kRsaModulus) == 0 &&
        mbedtls_mpi_exp_mod(&r, &m, &e, &n, nullptr) == 0) {
        unsigned char out[128];
        if (mbedtls_mpi_write_binary(&r, out, sizeof(out)) == 0) {
            enc = hexUpper(std::vector<unsigned char>(out, out + sizeof(out)));
        }
    }

    mbedtls_mpi_free(&m);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&r);
    return enc;
}

} // namespace

std::string randomBase62(size_t len) {
    std::vector<unsigned char> raw(len);
    randomGet(raw.data(), raw.size());
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kBase62[raw[i] % 62]);
    }
    return out;
}

WeapiParams weapiEncrypt(const nlohmann::json &body) {
    std::string text = body.dump();
    std::string secret = randomBase62(16);

    WeapiParams out;
    out.params = aesCbcBase64(aesCbcBase64(text, kPresetKey), secret);
    out.enc_sec_key = rsaEncrypt(secret);
    return out;
}

} // namespace ntv::crypto
