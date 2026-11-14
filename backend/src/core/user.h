#pragma once

#include <cstdint>
#include <string>

namespace englishword {

/// 账号实体：只描述数据，不依赖 Drogon，便于单元测试。
struct User {
    std::int64_t id = 0;      ///< 用户编号，对应 users.id
    std::string username;     ///< 登录名，对应 users.username
    std::string passwordHash; ///< 密码验证串，对应 users.password_hash，绝不存明文
    std::string createdAt;    ///< 注册时间，对应 users.created_at
};

} // namespace englishword

