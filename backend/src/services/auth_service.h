#pragma once

#include <string>

namespace englishword {

/// 密码哈希与校验；哈希串自带算法参数，便于以后升级迭代次数。
class AuthService {
public:
    /// 生成带随机盐的验证串，格式：pbkdf2$<迭代次数>$<盐hex>$<哈希hex>。
    static std::string hashPassword(const std::string& plain);

    /// 校验明文是否与验证串匹配；验证串格式非法时返回 false，不抛异常。
    static bool verifyPassword(const std::string& plain, const std::string& stored);
};

} // namespace englishword
