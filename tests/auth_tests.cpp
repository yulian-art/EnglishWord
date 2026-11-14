#include "services/auth_service.h"

#include <iostream>
#include <stdexcept>
#include <string>

int main()
{
    try {
        const auto first = englishword::AuthService::hashPassword("secret-123");  // 第一次哈希。
        const auto second = englishword::AuthService::hashPassword("secret-123"); // 第二次哈希。
        if (first == second) {
            throw std::runtime_error("同一密码两次哈希应因随机盐而不同。");
        }
        if (first.find("secret-123") != std::string::npos) {
            throw std::runtime_error("哈希串不能包含明文密码。");
        }
        if (first.rfind("pbkdf2$100000$", 0) != 0) {
            throw std::runtime_error("哈希串格式应为 pbkdf2$迭代次数$盐$哈希。");
        }
        if (!englishword::AuthService::verifyPassword("secret-123", first)) {
            throw std::runtime_error("正确密码应校验通过。");
        }
        if (englishword::AuthService::verifyPassword("secret-124", first)) {
            throw std::runtime_error("错误密码不应通过。");
        }
        if (englishword::AuthService::verifyPassword("secret-123", "not-a-hash")) {
            throw std::runtime_error("非法格式应返回 false，不能抛异常。");
        }
        std::cout << "Password hashing rules passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
