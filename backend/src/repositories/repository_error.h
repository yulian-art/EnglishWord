#pragma once

#include <stdexcept>
#include <string>

namespace englishword {

/// 仓储错误不依赖 HTTP；由控制器将资源、权限和唯一性错误映射为状态码。
class RepositoryError : public std::runtime_error {
public:
    enum class Kind { NotFound, Forbidden, Conflict };

    /// kind 标识错误类别，message 是可向用户展示的说明。
    RepositoryError(Kind kind, const std::string& message)
        : std::runtime_error(message), kind(kind) {}
    Kind kind; ///< 资源不存在、禁止访问或数据冲突。
};

} // namespace englishword
