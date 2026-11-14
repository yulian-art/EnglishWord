#pragma once

#include "core/user.h"

#include <drogon/orm/DbClient.h>

#include <cstdint>
#include <optional>
#include <string>

namespace englishword {

/// 用户表读写，所有 SQL 都使用参数占位符。
class UserRepository {
public:
    /// 打开 path 对应的 SQLite 文件；表由 WordRepository 的迁移负责创建。
    explicit UserRepository(const std::string& path);

    /// 按用户名查用户，不存在返回 std::nullopt。
    std::optional<User> findByUsername(const std::string& username) const;

    /// 按编号查用户，不存在返回 std::nullopt。
    std::optional<User> findById(std::int64_t id) const;

    /// 插入用户并返回新编号；用户名重复时抛 drogon::orm::DrogonDbException。
    std::int64_t create(const std::string& username, const std::string& passwordHash) const;

private:
    drogon::orm::DbClientPtr m_client; ///< Drogon 管理的 SQLite 连接。
};

} // namespace englishword
