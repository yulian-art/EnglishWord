#include "repositories/word_repository.h"
#include "resources.h"

#include <algorithm>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace englishword {
namespace {

constexpr std::size_t maxMeanings = 50;
constexpr std::size_t maxImportBytes = 48 * 1024;
constexpr std::size_t maxImportLines = 2000;

/// 仅处理 ASCII 空白；不能逐字节调用 locale 的 isspace 破坏 UTF-8。
bool asciiSpace(unsigned char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

std::string trimAscii(const std::string& text)
{
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && asciiSpace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    while (last > first && asciiSpace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return text.substr(first, last - first);
}

/// 去重键与 SQLite NOCASE 一致，仅折叠 ASCII 大小写，不改变中文等 Unicode 字符。
std::string duplicateKey(const std::string& text)
{
    auto key = trimAscii(text);
    for (auto& ch : key) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return key;
}

/// 严格解码 Unicode 标量值；拒绝截断、过长编码、孤立续字节、代理区和越界码点。
std::vector<std::uint32_t> decodeUtf8(const std::string& text)
{
    std::vector<std::uint32_t> points;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        std::size_t trailing = 0;
        if (lead < 0x80) {
            value = lead;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            value = lead & 0x1f;
            minimum = 0x80;
            trailing = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            value = lead & 0x0f;
            minimum = 0x800;
            trailing = 2;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            value = lead & 0x07;
            minimum = 0x10000;
            trailing = 3;
        } else {
            throw std::invalid_argument("必须使用合法的 UTF-8 编码。");
        }
        if (trailing > text.size() - i) {
            throw std::invalid_argument("UTF-8 字符不完整。");
        }
        for (std::size_t n = 0; n < trailing; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) {
                throw std::invalid_argument("必须使用合法的 UTF-8 编码。");
            }
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            throw std::invalid_argument("UTF-8 包含非法 Unicode 码点。");
        }
        points.push_back(value);
    }
    return points;
}

/// 所有词库写入口共用码点长度、UTF-8 和控制字符规则。
std::string checkedText(const std::string& raw, const std::string& label,
                        std::size_t minimum, std::size_t maximum, bool english = false)
{
    const auto text = trimAscii(raw);
    const auto points = decodeUtf8(text);
    if (points.size() < minimum || points.size() > maximum) {
        throw std::invalid_argument(label + "长度必须为 " + std::to_string(minimum) + ".." +
                                    std::to_string(maximum) + " 个 Unicode 字符。");
    }
    bool hasEnglishLetter = false;
    for (const auto point : points) {
        if (point < 0x20 || (point >= 0x7f && point <= 0x9f)) {
            throw std::invalid_argument(label + "不能包含 NUL 或控制字符。");
        }
        hasEnglishLetter = hasEnglishLetter || (point >= 'A' && point <= 'Z') ||
                           (point >= 'a' && point <= 'z');
    }
    if (english && !hasEnglishLetter) {
        throw std::invalid_argument("英文必须至少包含一个 ASCII 英文字母。");
    }
    return text;
}

std::vector<std::string> checkedMeanings(const std::vector<std::string>& meanings)
{
    if (meanings.empty() || meanings.size() > maxMeanings) {
        throw std::invalid_argument("释义数组必须包含 1..50 条释义。");
    }
    std::vector<std::string> cleaned;
    std::unordered_set<std::string> seen;
    for (const auto& raw : meanings) {
        auto meaning = checkedText(raw, "释义", 1, 500);
        if (seen.insert(duplicateKey(meaning)).second) {
            cleaned.push_back(std::move(meaning));
        }
    }
    return cleaned;
}

/// Drogon 在析构时异步 COMMIT；必须等待提交回调，不能把排队成功当成提交成功。
class WriteTransaction {
public:
    explicit WriteTransaction(const drogon::orm::DbClientPtr& client)
        : m_client(client), m_completion(std::make_shared<std::promise<bool>>()),
          m_result(m_completion->get_future()),
          m_transaction(client->newTransaction([completion = m_completion](bool committed) {
              completion->set_value(committed);
          }))
    {
    }

    ~WriteTransaction()
    {
        if (m_transaction) {
            m_transaction->rollback();
            m_transaction.reset();
            // 单连接队列屏障：异常返回前确保回滚完成，保留原始业务/SQL 异常。
            try {
                m_client->execSqlSync("SELECT 1");
            } catch (...) {
            }
        }
    }

    drogon::orm::DbClient& db() { return *m_transaction; }

    void commit()
    {
        m_transaction.reset();
        if (!m_result.get()) {
            // SQLite 的 COMMIT 失败可能仍保留活动事务；兼容未自动回滚的 Drogon 版本。
            // 此仓储只有一个连接。已被驱动回滚时 ROLLBACK 会报“无事务”，可安全忽略。
            try {
                m_client->execSqlSync("ROLLBACK");
            } catch (...) {
            }
            throw std::runtime_error("数据库事务提交失败，未保存本次修改。");
        }
    }

private:
    drogon::orm::DbClientPtr m_client;
    std::shared_ptr<std::promise<bool>> m_completion;
    std::future<bool> m_result;
    std::shared_ptr<drogon::orm::Transaction> m_transaction;
};

void requireOwner(const drogon::orm::Field& owner, std::int64_t userId, bool write)
{
    if (owner.isNull()) {
        if (write) {
            throw RepositoryError(RepositoryError::Kind::Forbidden, "公共示例词库只读。");
        }
    } else if (userId <= 0 || owner.as<std::int64_t>() != userId) {
        throw RepositoryError(RepositoryError::Kind::Forbidden, "无权访问此私人词库。");
    }
}

void requireBook(drogon::orm::DbClient& db, std::int64_t bookId, std::int64_t userId, bool write)
{
    const auto rows = db.execSqlSync("SELECT owner_user_id FROM wordbooks WHERE id = ?", bookId);
    if (rows.empty()) {
        throw RepositoryError(RepositoryError::Kind::NotFound, "词库不存在。");
    }
    requireOwner(rows[0]["owner_user_id"], userId, write);
}

std::vector<Word> collectWords(const drogon::orm::Result& rows)
{
    std::vector<Word> words;
    for (const auto& row : rows) {
        if (row["id"].isNull()) {
            continue; // LEFT JOIN 保留的空词库行。
        }
        const auto id = row["id"].as<std::int64_t>();
        if (words.empty() || words.back().id != id) {
            Word word;
            word.id = id;
            word.english = row["english"].as<std::string>();
            word.partOfSpeech = row["part_of_speech"].as<std::string>();
            words.push_back(std::move(word));
        }
        if (!row["meaning"].isNull()) {
            words.back().meanings.push_back(row["meaning"].as<std::string>());
        }
    }
    return words;
}

Word loadWord(drogon::orm::DbClient& db, std::int64_t wordId, std::int64_t userId, bool write)
{
    const auto rows = db.execSqlSync(
        "SELECT w.id, w.english, w.part_of_speech, b.owner_user_id, m.meaning FROM words w "
        "JOIN wordbooks b ON b.id = w.wordbook_id "
        "LEFT JOIN word_meanings m ON m.word_id = w.id WHERE w.id = ? ORDER BY m.id", wordId);
    if (rows.empty()) {
        throw RepositoryError(RepositoryError::Kind::NotFound, "单词不存在。");
    }
    requireOwner(rows[0]["owner_user_id"], userId, write);
    return collectWords(rows).front();
}

void requireUniqueEnglish(drogon::orm::DbClient& db, std::int64_t bookId,
                           const std::string& english, std::int64_t exceptWordId = 0)
{
    const auto rows = db.execSqlSync(
        "SELECT id FROM words WHERE wordbook_id = ? AND "
        "trim(english, char(9, 10, 11, 12, 13, 32)) = ? COLLATE NOCASE AND id <> ?",
        bookId, english, exceptWordId);
    if (!rows.empty()) {
        throw RepositoryError(RepositoryError::Kind::Conflict, "此词库已存在相同的英文单词。");
    }
}

std::int64_t insertWord(drogon::orm::DbClient& db, std::int64_t bookId,
                        const std::string& english, const std::string& partOfSpeech)
{
    db.execSqlSync("INSERT INTO words(wordbook_id, english, part_of_speech) VALUES (?, ?, ?)",
                   bookId, english, partOfSpeech);
    return db.execSqlSync("SELECT last_insert_rowid() AS id")[0]["id"].as<std::int64_t>();
}

void appendImportError(Json::Value& errors, std::size_t line, const std::string& message)
{
    Json::Value error;
    error["line"] = Json::UInt64(line);
    error["message"] = message;
    errors.append(std::move(error));
}

struct ImportRow {
    std::size_t line;
    std::string english;
    std::string partOfSpeech;
    std::string meaning;
};

/// 解析阶段只验证输入，不接触写事务；保留空行在内的物理行号。
std::vector<ImportRow> parseTsv(const std::string& content)
{
    Json::Value errors(Json::arrayValue);
    if (content.size() > maxImportBytes) {
        appendImportError(errors, 1, "TSV 内容不能超过 48 KiB。");
        throw ImportError(std::move(errors));
    }
    auto text = content;
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) {
        text.erase(0, 3);
    }
    std::istringstream source(text);
    std::string line;
    std::size_t lineNumber = 0;
    bool headerSeen = false;
    std::vector<ImportRow> rows;
    while (std::getline(source, line)) {
        ++lineNumber;
        if (lineNumber > maxImportLines) {
            appendImportError(errors, lineNumber, "TSV 最多允许 2000 行（包括表头和空行）。");
            break;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back(); // 支持 CRLF；其他位置的 CR 仍属于非法控制字符。
        }
        if (line.find('\t') == std::string::npos && trimAscii(line).empty()) {
            continue;
        }
        try {
            decodeUtf8(line);
        } catch (const std::invalid_argument& error) {
            appendImportError(errors, lineNumber, error.what());
            if (!headerSeen) {
                headerSeen = true;
            }
            continue;
        }
        if (!headerSeen) {
            headerSeen = true;
            if (line != "english\tpos\tmeaning") {
                appendImportError(errors, lineNumber, "表头必须为 english<TAB>pos<TAB>meaning。");
            }
            continue;
        }
        const auto firstTab = line.find('\t');
        const auto secondTab = firstTab == std::string::npos ? std::string::npos
                                                            : line.find('\t', firstTab + 1);
        if (secondTab == std::string::npos || line.find('\t', secondTab + 1) != std::string::npos) {
            appendImportError(errors, lineNumber, "每条数据必须恰好包含 english、pos、meaning 三列。");
            continue;
        }
        ImportRow row{lineNumber, {}, {}, {}};
        const auto checkField = [&](const std::string& raw, const std::string& label,
                                    std::size_t minimum, std::size_t maximum, bool english) {
            try {
                return checkedText(raw, label, minimum, maximum, english);
            } catch (const std::invalid_argument& error) {
                appendImportError(errors, lineNumber, error.what());
                return std::string();
            }
        };
        const auto before = errors.size();
        row.english = checkField(line.substr(0, firstTab), "英文", 1, 100, true);
        row.partOfSpeech = checkField(line.substr(firstTab + 1, secondTab - firstTab - 1),
                                      "词性", 0, 50, false);
        row.meaning = checkField(line.substr(secondTab + 1), "释义", 1, 500, false);
        if (before == errors.size()) {
            rows.push_back(std::move(row));
        }
    }
    if (!headerSeen) {
        appendImportError(errors, 1, "缺少 english<TAB>pos<TAB>meaning 表头。");
    }
    if (!errors.empty()) {
        throw ImportError(std::move(errors));
    }
    return rows;
}

/// 执行无触发器的迁移脚本，忽略注释/引号内部的分号，支持 SQL 引号转义。
void executeMigration(drogon::orm::DbClient& db, const std::string& script)
{
    std::string statement;
    char quote = 0;
    bool lineComment = false;
    bool blockComment = false;
    bool hasSql = false;
    for (std::size_t i = 0; i < script.size(); ++i) {
        const auto ch = script[i];
        const auto next = i + 1 < script.size() ? script[i + 1] : '\0';
        statement += ch;
        if (lineComment) {
            if (ch == '\n') {
                lineComment = false;
            }
        } else if (blockComment) {
            if (ch == '*' && next == '/') {
                statement += script[++i];
                blockComment = false;
            }
        } else if (quote) {
            if (ch == quote) {
                if (next == quote) {
                    statement += script[++i];
                } else {
                    quote = 0;
                }
            }
        } else if (ch == '-' && next == '-') {
            statement += script[++i];
            lineComment = true;
        } else if (ch == '/' && next == '*') {
            statement += script[++i];
            blockComment = true;
        } else if (ch == '\'' || ch == '"' || ch == '`' || ch == '[') {
            quote = ch == '[' ? ']' : ch;
            hasSql = true;
        } else if (ch == ';') {
            if (hasSql) {
                db.execSqlSync(statement);
            }
            statement.clear();
            hasSql = false;
        } else if (!asciiSpace(static_cast<unsigned char>(ch))) {
            hasSql = true;
        }
    }
    if (hasSql) {
        db.execSqlSync(statement);
    }
}

} // namespace

ImportError::ImportError(Json::Value errors)
    : std::invalid_argument("TSV 导入校验失败。"), errors(std::move(errors))
{
}

WordRepository::WordRepository(const std::string& path)
    : m_client(drogon::orm::DbClient::newSqlite3Client("filename=" + path, 1))
{
    if (!m_client) {
        throw std::runtime_error("Drogon 未启用 SQLite 支持。");
    }
    initialize();
}

void WordRepository::initialize()
{
    m_client->execSqlSync("PRAGMA foreign_keys = ON");
    m_client->execSqlSync("PRAGMA busy_timeout = 5000");
    WriteTransaction transaction(m_client);
    auto& db = transaction.db();
    auto version = db.execSqlSync("PRAGMA user_version")[0][0].as<int>();
    if (version > 2) {
        throw std::runtime_error("数据库版本高于当前程序支持的版本。");
    }
    if (version == 0) {
        executeMigration(db, resources::schema);
        db.execSqlSync("INSERT INTO wordbooks(name) VALUES (?)", "入门示例词库");
        const auto bookId = db.execSqlSync("SELECT last_insert_rowid() AS id")[0]["id"]
                                .as<std::int64_t>();
        for (const auto& row : parseTsv(resources::demoTsv)) {
            const auto existing = db.execSqlSync(
                "SELECT id FROM words WHERE wordbook_id = ? AND english = ? COLLATE NOCASE",
                bookId, row.english);
            const auto wordId = existing.empty() ? insertWord(db, bookId, row.english, row.partOfSpeech)
                                                 : existing[0]["id"].as<std::int64_t>();
            db.execSqlSync("INSERT OR IGNORE INTO word_meanings(word_id, meaning) VALUES (?, ?)",
                           wordId, row.meaning);
        }
        db.execSqlSync("PRAGMA user_version = 1");
        version = 1;
    }
    if (version == 1) {
        executeMigration(db, resources::progressSchema);
        db.execSqlSync("PRAGMA user_version = 2");
    }
    transaction.commit();
}

Json::Value WordRepository::listDemoWordbooks() const
{
    const auto rows = m_client->execSqlSync(
        "SELECT b.id, b.name, COUNT(w.id) AS word_count FROM wordbooks b "
        "LEFT JOIN words w ON w.wordbook_id = b.id WHERE b.owner_user_id IS NULL "
        "GROUP BY b.id, b.name ORDER BY b.id");
    Json::Value books(Json::arrayValue);
    for (const auto& row : rows) {
        Json::Value book;
        book["id"] = Json::Int64(row["id"].as<std::int64_t>());
        book["name"] = row["name"].as<std::string>();
        book["wordCount"] = row["word_count"].as<int>();
        book["isDemo"] = true;
        books.append(std::move(book));
    }
    return books;
}

std::vector<Word> WordRepository::listDemoWords(std::int64_t bookId) const
{
    return collectWords(m_client->execSqlSync(
        "SELECT w.id, w.english, w.part_of_speech, m.meaning FROM words w "
        "JOIN wordbooks b ON b.id = w.wordbook_id "
        "LEFT JOIN word_meanings m ON m.word_id = w.id "
        "WHERE b.id = ? AND b.owner_user_id IS NULL ORDER BY w.id, m.id", bookId));
}

Json::Value WordRepository::listWordbooksForUser(std::int64_t userId) const
{
    const auto rows = m_client->execSqlSync(
        "SELECT b.id, b.name, b.owner_user_id, COUNT(w.id) AS word_count FROM wordbooks b "
        "LEFT JOIN words w ON w.wordbook_id = b.id "
        "WHERE b.owner_user_id IS NULL OR (b.owner_user_id = ? AND ? > 0) "
        "GROUP BY b.id, b.name, b.owner_user_id ORDER BY b.id", userId, userId);
    Json::Value books(Json::arrayValue);
    for (const auto& row : rows) {
        Json::Value book;
        book["id"] = Json::Int64(row["id"].as<std::int64_t>());
        book["name"] = row["name"].as<std::string>();
        book["wordCount"] = row["word_count"].as<int>();
        book["isDemo"] = row["owner_user_id"].isNull();
        books.append(std::move(book));
    }
    return books;
}

std::int64_t WordRepository::createWordbook(const std::string& name, std::int64_t ownerId) const
{
    const auto cleanName = checkedText(name, "词库名称", 1, 50);
    WriteTransaction transaction(m_client);
    auto& db = transaction.db();
    if (ownerId <= 0 || db.execSqlSync("SELECT id FROM users WHERE id = ?", ownerId).empty()) {
        throw RepositoryError(RepositoryError::Kind::Forbidden, "创建私人词库需要有效的登录用户。");
    }
    db.execSqlSync("INSERT INTO wordbooks(owner_user_id, name) VALUES (?, ?)", ownerId, cleanName);
    const auto id = db.execSqlSync("SELECT last_insert_rowid() AS id")[0]["id"].as<std::int64_t>();
    transaction.commit();
    return id;
}

void WordRepository::requireWordbook(std::int64_t bookId, std::int64_t userId, bool write) const
{
    requireBook(*m_client, bookId, userId, write);
}

std::vector<Word> WordRepository::listWords(std::int64_t bookId, std::int64_t userId) const
{
    const auto rows = m_client->execSqlSync(
        "SELECT w.id, w.english, w.part_of_speech, b.owner_user_id, m.meaning FROM wordbooks b "
        "LEFT JOIN words w ON w.wordbook_id = b.id "
        "LEFT JOIN word_meanings m ON m.word_id = w.id WHERE b.id = ? ORDER BY w.id, m.id", bookId);
    if (rows.empty()) {
        throw RepositoryError(RepositoryError::Kind::NotFound, "词库不存在。");
    }
    requireOwner(rows[0]["owner_user_id"], userId, false);
    return collectWords(rows);
}

Word WordRepository::getWord(std::int64_t wordId, std::int64_t userId, bool write) const
{
    return loadWord(*m_client, wordId, userId, write);
}

Word WordRepository::createWord(std::int64_t bookId, std::int64_t userId,
                                 const std::string& english, const std::string& partOfSpeech,
                                 const std::vector<std::string>& meanings) const
{
    const auto cleanEnglish = checkedText(english, "英文", 1, 100, true);
    const auto cleanPos = checkedText(partOfSpeech, "词性", 0, 50);
    const auto cleanMeanings = checkedMeanings(meanings);
    WriteTransaction transaction(m_client);
    auto& db = transaction.db();
    requireBook(db, bookId, userId, true);
    requireUniqueEnglish(db, bookId, cleanEnglish);
    const auto wordId = insertWord(db, bookId, cleanEnglish, cleanPos);
    for (const auto& meaning : cleanMeanings) {
        db.execSqlSync("INSERT INTO word_meanings(word_id, meaning) VALUES (?, ?)", wordId, meaning);
    }
    auto word = loadWord(db, wordId, userId, true);
    transaction.commit();
    return word;
}

Word WordRepository::updateWord(std::int64_t wordId, std::int64_t userId,
                                 const std::optional<std::string>& english,
                                 const std::optional<std::string>& partOfSpeech) const
{
    if (!english && !partOfSpeech) {
        throw std::invalid_argument("至少提供英文或词性中的一个字段。");
    }
    const auto cleanEnglish = english ? checkedText(*english, "英文", 1, 100, true) : std::string();
    const auto cleanPos = partOfSpeech ? checkedText(*partOfSpeech, "词性", 0, 50) : std::string();
    WriteTransaction transaction(m_client);
    auto& db = transaction.db();
    auto word = loadWord(db, wordId, userId, true);
    if (english) {
        const auto bookId = db.execSqlSync("SELECT wordbook_id FROM words WHERE id = ?", wordId)
                                [0]["wordbook_id"].as<std::int64_t>();
        requireUniqueEnglish(db, bookId, cleanEnglish, wordId);
        word.english = cleanEnglish;
    }
    if (partOfSpeech) {
        word.partOfSpeech = cleanPos;
    }
    db.execSqlSync("UPDATE words SET english = ?, part_of_speech = ? WHERE id = ?",
                   word.english, word.partOfSpeech, wordId);
    transaction.commit();
    return word;
}

void WordRepository::deleteWord(std::int64_t wordId, std::int64_t userId) const
{
    WriteTransaction transaction(m_client);
    auto& db = transaction.db();
    loadWord(db, wordId, userId, true);
    db.execSqlSync("DELETE FROM words WHERE id = ?", wordId);
    transaction.commit();
}

Word WordRepository::addMeaning(std::int64_t wordId, std::int64_t userId,
                                const std::string& meaning) const
{
    const auto cleanMeaning = checkedText(meaning, "释义", 1, 500);
    WriteTransaction transaction(m_client);
    auto& db = transaction.db();
    auto word = loadWord(db, wordId, userId, true);
    const auto key = duplicateKey(cleanMeaning);
    const bool duplicate = std::any_of(word.meanings.begin(), word.meanings.end(),
                                      [&](const auto& existing) { return duplicateKey(existing) == key; });
    if (!duplicate) {
        if (word.meanings.size() >= maxMeanings) {
            throw std::invalid_argument("一个单词最多允许 50 条不同释义。");
        }
        db.execSqlSync("INSERT INTO word_meanings(word_id, meaning) VALUES (?, ?)", wordId, cleanMeaning);
        word.meanings.push_back(cleanMeaning);
    }
    transaction.commit();
    return word;
}

Json::Value WordRepository::importTsv(std::int64_t bookId, std::int64_t userId,
                                      const std::string& content,
                                      const std::string& duplicatePolicy) const
{
    // 先授权再暴露输入错误；事务内仍重新校验，绝不缓存所有权。
    requireBook(*m_client, bookId, userId, true);
    if (duplicatePolicy != "merge" && duplicatePolicy != "skip") {
        throw std::invalid_argument("duplicatePolicy 只能为 merge 或 skip。");
    }
    const auto rows = parseTsv(content);
    WriteTransaction transaction(m_client);
    auto& db = transaction.db();
    requireBook(db, bookId, userId, true);

    struct State {
        Word word;
        std::string originalPos;
        std::unordered_set<std::string> meaningKeys;
        std::vector<std::string> addedMeanings;
    };
    std::vector<State> states;
    std::unordered_map<std::string, std::size_t> indices;
    const auto existingWords = collectWords(db.execSqlSync(
        "SELECT w.id, w.english, w.part_of_speech, m.meaning FROM words w "
        "LEFT JOIN word_meanings m ON m.word_id = w.id "
        "WHERE w.wordbook_id = ? ORDER BY w.id, m.id", bookId));
    for (const auto& word : existingWords) {
        State state;
        state.word = word;
        state.originalPos = word.partOfSpeech;
        for (const auto& meaning : word.meanings) {
            state.meaningKeys.insert(duplicateKey(meaning));
        }
        indices.emplace(duplicateKey(word.english), states.size());
        states.push_back(std::move(state));
    }
    Json::Value summary;
    summary["imported"] = 0;
    summary["merged"] = 0;
    summary["skipped"] = 0;
    Json::Value errors(Json::arrayValue);
    // 先在内存模拟整批合并，连超过 50 条释义的错误也不会产生中间写入。
    for (const auto& row : rows) {
        const auto key = duplicateKey(row.english);
        auto found = indices.find(key);
        bool inserted = found == indices.end();
        if (inserted) {
            State state;
            state.word.english = row.english;
            state.word.partOfSpeech = row.partOfSpeech;
            indices.emplace(key, states.size());
            states.push_back(std::move(state));
            found = indices.find(key);
        } else if (duplicatePolicy == "skip") {
            summary["skipped"] = summary["skipped"].asInt() + 1;
            continue;
        }
        auto& state = states[found->second];
        bool changed = false;
        if (state.word.partOfSpeech.empty() && !row.partOfSpeech.empty()) {
            state.word.partOfSpeech = row.partOfSpeech;
            changed = true;
        }
        if (state.meaningKeys.find(duplicateKey(row.meaning)) == state.meaningKeys.end()) {
            if (state.meaningKeys.size() >= maxMeanings) {
                appendImportError(errors, row.line, "一个单词最多允许 50 条不同释义。");
                continue;
            }
            state.meaningKeys.insert(duplicateKey(row.meaning));
            state.addedMeanings.push_back(row.meaning);
            changed = true;
        }
        const char* counter = inserted ? "imported" : changed ? "merged" : "skipped";
        summary[counter] = summary[counter].asInt() + 1;
    }
    if (!errors.empty()) {
        throw ImportError(std::move(errors));
    }
    for (auto& state : states) {
        if (state.word.id == 0) {
            state.word.id = insertWord(db, bookId, state.word.english, state.word.partOfSpeech);
        } else if (state.originalPos != state.word.partOfSpeech) {
            db.execSqlSync("UPDATE words SET part_of_speech = ? WHERE id = ?",
                           state.word.partOfSpeech, state.word.id);
        }
        for (const auto& meaning : state.addedMeanings) {
            db.execSqlSync("INSERT INTO word_meanings(word_id, meaning) VALUES (?, ?)",
                           state.word.id, meaning);
        }
    }
    transaction.commit();
    return summary;
}

} // namespace englishword
