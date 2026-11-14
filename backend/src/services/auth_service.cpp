#include "services/auth_service.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace englishword {
namespace {

constexpr int kIterations = 100000;    ///< PBKDF2 迭代次数，越大越慢越难爆破。
constexpr std::size_t kSaltBytes = 16; ///< 随机盐长度，每个用户不同。
constexpr std::size_t kHashBytes = 32; ///< 输出长度，SHA-256 固定 32 字节。

/// 字节串转小写十六进制，方便存进 TEXT 字段。
std::string toHex(const unsigned char* data, std::size_t size)
{
    static const char* digits = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        text.push_back(digits[data[index] >> 4]);
        text.push_back(digits[data[index] & 0x0F]);
    }
    return text;
}

/// 十六进制转字节串；出现非法字符返回 false。
bool fromHex(const std::string& text, std::vector<unsigned char>& out)
{
    if (text.size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        int value = 0;
        for (int offset = 0; offset < 2; ++offset) {
            const char character = text[index + offset];
            value <<= 4;
            if (character >= '0' && character <= '9') {
                value += character - '0';
            } else if (character >= 'a' && character <= 'f') {
                value += character - 'a' + 10;
            } else if (character >= 'A' && character <= 'F') {
                value += character - 'A' + 10;
            } else {
                return false;
            }
        }
        out.push_back(static_cast<unsigned char>(value));
    }
    return true;
}

/// 用 PBKDF2-HMAC-SHA256 派生哈希，返回十六进制字符串。
std::string derive(const std::string& plain, const std::vector<unsigned char>& salt, int iterations)
{
    std::vector<unsigned char> hash(kHashBytes);
    if (PKCS5_PBKDF2_HMAC(plain.data(), static_cast<int>(plain.size()), salt.data(),
                          static_cast<int>(salt.size()), iterations, EVP_sha256(),
                          static_cast<int>(hash.size()), hash.data()) != 1) {
        throw std::runtime_error("PBKDF2 计算失败。");
    }
    return toHex(hash.data(), hash.size());
}

} // namespace

std::string AuthService::hashPassword(const std::string& plain)
{
    std::vector<unsigned char> salt(kSaltBytes);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw std::runtime_error("生成随机盐失败。");
    }
    return "pbkdf2$" + std::to_string(kIterations) + "$" + toHex(salt.data(), salt.size()) + "$" +
           derive(plain, salt, kIterations);
}

bool AuthService::verifyPassword(const std::string& plain, const std::string& stored)
{
    std::vector<std::string> parts; // 按 $ 切出的四段：算法、迭代次数、盐、哈希。
    std::size_t start = 0;
    while (true) {
        const auto position = stored.find('$', start);
        if (position == std::string::npos) {
            parts.push_back(stored.substr(start));
            break;
        }
        parts.push_back(stored.substr(start, position - start));
        start = position + 1;
    }
    if (parts.size() != 4 || parts[0] != "pbkdf2") {
        return false;
    }
    int iterations = 0;
    try {
        iterations = std::stoi(parts[1]);
    } catch (const std::exception&) {
        return false;
    }
    if (iterations <= 0) {
        return false;
    }
    std::vector<unsigned char> salt;
    if (!fromHex(parts[2], salt) || salt.empty()) {
        return false;
    }
    const auto expected = derive(plain, salt, iterations);
    // 恒定时间比较，避免用响应时间猜出哈希前缀。
    return expected.size() == parts[3].size() &&
           CRYPTO_memcmp(expected.data(), parts[3].data(), expected.size()) == 0;
}

} // namespace englishword
