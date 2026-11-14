#pragma once

#include "core/word.h"
#include "repositories/repository_error.h"

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace englishword {

/// TSV 输入错误；errors 按实际输入行号给出 {line, message}，不会写入任何数据。
class ImportError : public std::invalid_argument {
public:
    explicit ImportError(Json::Value errors);
    Json::Value errors; ///< 包括表头、UTF-8、字段和行数限制在内的错误数组。
};

/// 通过 Drogon ORM 数据库客户端执行参数化 SQL，避免自行封装 SQLite C API。
class WordRepository {
public:
    /// 打开 path 对应的 SQLite 文件，迁移至版本 2 并初始化公共示例；失败抛异常。
    explicit WordRepository(const std::string& path);
    /// 返回公共示例词库的 JSON 数组。
    Json::Value listDemoWordbooks() const;
    /// 兼容示例查询；非公共或不存在的词库返回空数组。
    std::vector<Word> listDemoWords(std::int64_t bookId) const;
    /// 返回公共词库和 userId 的私有词库；userId 为 0 表示未登录，只返回公共。
    Json::Value listWordbooksForUser(std::int64_t userId) const;
    /// 创建属于 ownerId 的私有词库，确认提交后返回新编号。
    std::int64_t createWordbook(const std::string& name, std::int64_t ownerId) const;
    /// 每次验证存在性和所有权；公共示例匿名可读，但任何用户均不可写。
    void requireWordbook(std::int64_t bookId, std::int64_t userId, bool write = false) const;
    /// 查询可访问的词库和全部释义；空词库返回空数组，不存在/无权限抛仓储错误。
    std::vector<Word> listWords(std::int64_t bookId, std::int64_t userId) const;
    /// 查询单词及全部释义；write 为 true 时同时要求所属私有词库可写。
    Word getWord(std::int64_t wordId, std::int64_t userId, bool write = false) const;
    /// 新增单词和 1..50 条释义；同词库英文忽略 ASCII 大小写和两端空白去重。
    Word createWord(std::int64_t bookId, std::int64_t userId, const std::string& english,
                    const std::string& partOfSpeech,
                    const std::vector<std::string>& meanings) const;
    /// 仅更新提供的字段；至少提供一个字段，不能与同词库其他英文重复。
    Word updateWord(std::int64_t wordId, std::int64_t userId,
                    const std::optional<std::string>& english,
                    const std::optional<std::string>& partOfSpeech) const;
    /// 删除单词；依赖外键级联删除释义和错题，历史明细保留且单词引用置空。
    void deleteWord(std::int64_t wordId, std::int64_t userId) const;
    /// 补充释义；重复释义不新增，一个单词最多保留 50 条不同释义。
    Word addMeaning(std::int64_t wordId, std::int64_t userId, const std::string& meaning) const;
    /// 验证 UTF-8 TSV 后整批提交；merge 新增释义/填空词性，skip 保留已有词。
    /// imported/merged/skipped 均按数据行计数；无新增信息的 merge 行计入 skipped。
    Json::Value importTsv(std::int64_t bookId, std::int64_t userId,
                          const std::string& content, const std::string& duplicatePolicy) const;

private:
    /// 版本 0 执行 001 + 示例，版本 1 执行 002；版本号与迁移在同一事务提交。
    void initialize();
    drogon::orm::DbClientPtr m_client; ///< Drogon 管理的单连接 SQLite 客户端。
};

} // namespace englishword
