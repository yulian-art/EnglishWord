#include "repositories/word_repository.h"
#include "repositories/user_repository.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using englishword::ImportError;
using englishword::RepositoryError;
using englishword::Word;
using englishword::WordRepository;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void expectKind(RepositoryError::Kind kind, Function&& function)
{
    try {
        function();
    } catch (const RepositoryError& error) {
        check(error.kind == kind, "仓储错误类别不正确：" + std::string(error.what()));
        return;
    }
    throw std::runtime_error("预期的仓储错误没有发生。");
}

template <typename Function>
void expectInvalid(Function&& function)
{
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("非法参数未被拒绝。");
}

template <typename Function>
void expectFailure(Function&& function)
{
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("预期的事务/数据库失败没有发生。");
}

template <typename Function>
Json::Value expectImportError(Function&& function)
{
    try {
        function();
    } catch (const ImportError& error) {
        check(error.errors.isArray() && !error.errors.empty(), "导入错误必须包含非空数组。");
        for (const auto& item : error.errors) {
            check(item["line"].asUInt64() > 0 && !item["message"].asString().empty(),
                  "导入错误必须包含实际行号和消息。");
        }
        return error.errors;
    }
    throw std::runtime_error("错误的 TSV 没有被拒绝。");
}

bool hasLine(const Json::Value& errors, int line)
{
    for (const auto& error : errors) {
        if (error["line"].asInt() == line) {
            return true;
        }
    }
    return false;
}

void checkCounts(const Json::Value& summary, int imported, int merged, int skipped)
{
    check(summary["imported"].asInt() == imported && summary["merged"].asInt() == merged &&
              summary["skipped"].asInt() == skipped,
          "导入逐行统计不正确：" + summary.toStyledString());
}

Word findEnglish(const std::vector<Word>& words, const std::string& english)
{
    const auto found = std::find_if(words.begin(), words.end(),
                                    [&](const auto& word) { return word.english == english; });
    check(found != words.end(), "没有找到单词 " + english);
    return *found;
}

std::string repeat(const std::string& text, int count)
{
    std::string result;
    for (int i = 0; i < count; ++i) {
        result += text;
    }
    return result;
}

/// 析构晚于仓储/客户端，Windows 上也不会在 SQLite 仍打开时删除文件。
struct TempDatabase {
    std::filesystem::path path;
    explicit TempDatabase(const std::string& suffix)
    {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("englishword-test-" + std::to_string(unique) + "-" + suffix + ".sqlite");
    }
    ~TempDatabase()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
        std::filesystem::remove(path.string() + "-journal", ignored);
    }
};

drogon::orm::DbClientPtr openDatabase(const std::filesystem::path& path)
{
    auto db = drogon::orm::DbClient::newSqlite3Client("filename=" + path.string(), 1);
    db->execSqlSync("PRAGMA foreign_keys = ON");
    db->execSqlSync("PRAGMA busy_timeout = 5000");
    return db;
}

/// 从未修改的 001 创建真实版本 1，验证升级路径而不是仅把版本 2 的编号改回 1。
void createVersionOne(drogon::orm::DbClient& db)
{
    const auto source = std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "backend/db/migrations/001_init.sql";
    std::ifstream file(source);
    check(file.good(), "无法读取版本 1 测试迁移脚本。");
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::istringstream script(buffer.str());
    std::string statement;
    while (std::getline(script, statement, ';')) {
        if (statement.find_first_not_of(" \t\r\n") != std::string::npos) {
            db.execSqlSync(statement);
        }
    }
    db.execSqlSync("PRAGMA user_version = 1");
}

void testMigrations()
{
    TempDatabase old("v1");
    std::int64_t oldWordId = 0;
    {
        auto db = openDatabase(old.path);
        createVersionOne(*db);
        db->execSqlSync("INSERT INTO users(id, username, password_hash) VALUES (7, 'old-user', 'hash')");
        db->execSqlSync("INSERT INTO wordbooks(id, owner_user_id, name) VALUES (8, 7, '旧私人词库')");
        db->execSqlSync("INSERT INTO words(id, wordbook_id, english, part_of_speech) "
                        "VALUES (9, 8, 'persisted', 'adj.')");
        db->execSqlSync("INSERT INTO word_meanings(word_id, meaning) VALUES (9, '已保存')");
        oldWordId = 9;
    }
    for (int restart = 0; restart < 2; ++restart) {
        WordRepository repository(old.path.string());
        check(repository.getWord(oldWordId, 7).meanings == std::vector<std::string>{"已保存"},
              "版本 1 升级/重启丢失已有词库。");
        check(repository.listDemoWordbooks().empty(), "升级版本 1 不应再次插入公共示例。");
        auto db = openDatabase(old.path);
        check(db->execSqlSync("PRAGMA user_version")[0][0].as<int>() == 2,
              "版本 1 未升级到版本 2。");
        check(db->execSqlSync("PRAGMA foreign_key_check").empty(), "升级后外键不一致。");
    }

    TempDatabase newer("future");
    {
        auto db = openDatabase(newer.path);
        db->execSqlSync("PRAGMA user_version = 3");
        expectFailure([&] { WordRepository unsupported(newer.path.string()); });
        check(db->execSqlSync("PRAGMA user_version")[0][0].as<int>() == 3,
              "不支持的高版本数据库不应被修改。");
    }

    TempDatabase broken("migration-rollback");
    {
        auto db = openDatabase(broken.path);
        db->execSqlSync("CREATE TABLE wordbooks(id INTEGER PRIMARY KEY)");
        expectFailure([&] { WordRepository invalid(broken.path.string()); });
        check(db->execSqlSync("PRAGMA user_version")[0][0].as<int>() == 0,
              "迁移失败不应提升版本号。");
        check(db->execSqlSync("SELECT name FROM sqlite_master WHERE name = 'users'").empty(),
              "迁移失败必须撤销已成功执行的 DDL。");
    }
}

void testRepository()
{
    TempDatabase temporary("repository");
    std::int64_t aliceId = 0;
    std::int64_t privateBook = 0;
    std::int64_t retainedWord = 0;
    std::size_t persistedCount = 0;
    std::size_t demoCount = 0;
    {
        WordRepository repository(temporary.path.string());
        auto observer = openDatabase(temporary.path);
        englishword::UserRepository users(temporary.path.string());
        check(observer->execSqlSync("PRAGMA user_version")[0][0].as<int>() == 2,
              "新库应在同一初始化事务中升级到版本 2。");
        const auto demos = repository.listDemoWordbooks();
        check(demos.size() == 1, "新库应有一个公共示例词库。");
        const auto demoId = demos[0]["id"].asInt64();
        const auto demoWords = repository.listDemoWords(demoId);
        demoCount = demoWords.size();
        check(demoCount > 0 && demos[0]["wordCount"].asUInt64() == demoCount,
              "示例数量必须与当前嵌入 TSV 一致，不能硬编码旧的十词示例。");
        for (const auto& word : demoWords) {
            check(!word.meanings.empty(), "当前示例每个单词应至少有一条释义。");
        }
        check(repository.listWords(demoId, 0).size() == demoCount &&
                  repository.getWord(demoWords.front().id, 0).id == demoWords.front().id,
              "公共示例必须匿名可读。");
        check(repository.listDemoWords(-1).empty(), "兼容示例接口应对不存在词库返回空。");

        aliceId = users.create("alice", "hash-1");
        const auto bobId = users.create("bob", "hash-2");
        check(aliceId > 0 && bobId > 0, "用户编号必须为正整数。");
        const auto found = users.findByUsername("alice");
        check(found && found->username == "alice" && found->passwordHash == "hash-1" &&
                  !found->createdAt.empty() && users.findById(aliceId).has_value() &&
                  !users.findByUsername("nobody"), "既有用户查询回归。");
        expectFailure([&] { users.create("alice", "hash-3"); });

        privateBook = repository.createWordbook("  我的中文词库  ", aliceId);
        const auto bobBook = repository.createWordbook("Bob 的词库", bobId);
        check(observer->execSqlSync("SELECT name FROM wordbooks WHERE id = ?", privateBook)
                  [0]["name"].as<std::string>() == "我的中文词库", "词库名称未清理或返回前未提交。");
        check(repository.listWords(privateBook, aliceId).empty(), "私人空词库应返回空而非 404。");
        check(repository.listWordbooksForUser(0).size() == 1 &&
                  repository.listWordbooksForUser(aliceId).size() == 2 &&
                  repository.listWordbooksForUser(bobId).size() == 2,
              "词库列表没有按当前用户隔离。");
        check(repository.listDemoWords(privateBook).empty(), "示例接口不能泄露私人单词。");
        expectKind(RepositoryError::Kind::NotFound, [&] { repository.requireWordbook(-1, aliceId); });
        expectKind(RepositoryError::Kind::NotFound, [&] { repository.listWords(-1, aliceId); });
        expectKind(RepositoryError::Kind::Forbidden, [&] { repository.listWords(privateBook, 0); });
        expectKind(RepositoryError::Kind::Forbidden, [&] { repository.listWords(privateBook, bobId); });
        expectKind(RepositoryError::Kind::Forbidden, [&] { repository.requireWordbook(privateBook, bobId, true); });
        expectKind(RepositoryError::Kind::Forbidden, [&] { repository.requireWordbook(demoId, aliceId, true); });
        expectKind(RepositoryError::Kind::Forbidden, [&] { repository.createWordbook("匿名", 0); });
        expectKind(RepositoryError::Kind::Forbidden, [&] { repository.createWordbook("无效用户", 999999); });

        auto word = repository.createWord(privateBook, aliceId, "  Book  ", " n. ",
                                           {" 书 ", "书籍", "书", " BOOK ", "book"});
        check(word.english == "Book" && word.partOfSpeech == "n." && word.meanings.size() == 3,
              "新建单词未清理字段或未去重多释义。");
        check(observer->execSqlSync("SELECT id FROM words WHERE id = ?", word.id).size() == 1,
              "新建单词返回之前必须已提交。");
        retainedWord = word.id;
        const auto otherWord = repository.createWord(privateBook, aliceId, "water", "n.", {"水"});
        repository.createWord(bobBook, bobId, "book", "n.", {"另一用户的书"});
        expectKind(RepositoryError::Kind::Conflict, [&] {
            repository.createWord(privateBook, aliceId, " bOoK ", "v.", {"预订"});
        });
        expectKind(RepositoryError::Kind::Conflict, [&] {
            repository.updateWord(otherWord.id, aliceId, std::string(" BOOK "), std::nullopt);
        });
        word = repository.updateWord(word.id, aliceId, std::string(" book "), std::string(" v. "));
        check(word.english == "book" && word.partOfSpeech == "v." && word.meanings.size() == 3,
              "单词更新不正确或丢失多释义。");
        word = repository.updateWord(word.id, aliceId, std::nullopt, std::string(""));
        check(word.partOfSpeech.empty(), "词性应该允许清空。");
        check(repository.addMeaning(word.id, aliceId, " 书籍 ").meanings.size() == 3 &&
                  repository.addMeaning(word.id, aliceId, "bOoK").meanings.size() == 3,
              "重复释义不应新增。");
        check(repository.addMeaning(word.id, aliceId, " 预订 ").meanings.size() == 4,
              "补充释义未成功。");
        check(observer->execSqlSync("SELECT COUNT(*) FROM word_meanings WHERE word_id = ?", word.id)
                  [0][0].as<int>() == 4, "补充释义返回之前必须已提交。");

        const auto checkDeniedWrites = [&](std::int64_t bookId, std::int64_t targetWord,
                                            std::int64_t actingUser) {
            expectKind(RepositoryError::Kind::Forbidden, [&] {
                repository.createWord(bookId, actingUser, "denied", "", {"拒绝"});
            });
            expectKind(RepositoryError::Kind::Forbidden, [&] {
                repository.updateWord(targetWord, actingUser, std::string("denied"), std::nullopt);
            });
            expectKind(RepositoryError::Kind::Forbidden, [&] { repository.deleteWord(targetWord, actingUser); });
            expectKind(RepositoryError::Kind::Forbidden, [&] { repository.addMeaning(targetWord, actingUser, "拒绝"); });
            expectKind(RepositoryError::Kind::Forbidden, [&] { repository.getWord(targetWord, actingUser, true); });
            expectKind(RepositoryError::Kind::Forbidden, [&] {
                repository.importTsv(bookId, actingUser, "english\tpos\tmeaning\nnew\tn.\t新", "merge");
            });
        };
        checkDeniedWrites(demoId, demoWords.front().id, 0);
        checkDeniedWrites(demoId, demoWords.front().id, aliceId);
        checkDeniedWrites(privateBook, word.id, 0);
        checkDeniedWrites(privateBook, word.id, bobId);
        expectKind(RepositoryError::Kind::Forbidden, [&] { repository.getWord(word.id, bobId); });
        expectKind(RepositoryError::Kind::NotFound, [&] { repository.getWord(-1, aliceId); });
        expectKind(RepositoryError::Kind::NotFound, [&] { repository.deleteWord(-1, aliceId); });
        expectKind(RepositoryError::Kind::NotFound, [&] { repository.createWord(-1, aliceId, "new", "", {"新"}); });
        // 先成功读取再转移所有者，所有后续入口必须重新读取权限，不能使用缓存。
        observer->execSqlSync("UPDATE wordbooks SET owner_user_id = ? WHERE id = ?", bobId, privateBook);
        expectKind(RepositoryError::Kind::Forbidden, [&] { repository.listWords(privateBook, aliceId); });
        expectKind(RepositoryError::Kind::Forbidden, [&] { repository.getWord(word.id, aliceId); });
        check(repository.getWord(word.id, bobId).id == word.id, "转移所有者后新用户不能访问。");
        observer->execSqlSync("UPDATE wordbooks SET owner_user_id = ? WHERE id = ?", aliceId, privateBook);

        expectInvalid([&] { repository.updateWord(word.id, aliceId, std::nullopt, std::nullopt); });
        expectInvalid([&] { repository.createWordbook("  ", aliceId); });
        expectInvalid([&] { repository.createWordbook(repeat("中", 51), aliceId); });
        repository.createWordbook(repeat("中", 50), aliceId);
        expectInvalid([&] { repository.createWord(privateBook, aliceId, "中文123", "", {"义"}); });
        expectInvalid([&] { repository.createWord(privateBook, aliceId, "", "", {"义"}); });
        expectInvalid([&] { repository.createWord(privateBook, aliceId, std::string(101, 'a'), "", {"义"}); });
        expectInvalid([&] { repository.createWord(privateBook, aliceId, "long-pos", repeat("中", 51), {"义"}); });
        expectInvalid([&] { repository.createWord(privateBook, aliceId, "empty-array", "", {}); });
        expectInvalid([&] { repository.createWord(privateBook, aliceId, "long-array", "", std::vector<std::string>(51, "义")); });
        expectInvalid([&] { repository.createWord(privateBook, aliceId, "empty-meaning", "", {" "}); });
        expectInvalid([&] { repository.addMeaning(word.id, aliceId, repeat("义", 501)); });
        expectInvalid([&] { repository.addMeaning(word.id, aliceId, "中\t文"); });
        expectInvalid([&] { repository.updateWord(word.id, aliceId, std::string("a\x01" "b"), std::nullopt); });
        expectInvalid([&] { repository.createWordbook("中\xc2\x85文", aliceId); });
        const std::vector<std::string> badUtf8 = {
            std::string("\xc0\xaf", 2), std::string("\x80", 1), std::string("\xe2\x82", 2),
            std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
            std::string("\xe0\x80\xaf", 3), std::string("x\0y", 3)};
        for (const auto& bad : badUtf8) {
            expectInvalid([&] { repository.createWordbook(bad, aliceId); });
            expectInvalid([&] { repository.createWord(privateBook, aliceId, "a" + bad, "", {"义"}); });
            expectInvalid([&] { repository.addMeaning(word.id, aliceId, bad); });
        }
        const auto unicodeWord = repository.createWord(privateBook, aliceId, "a" + repeat("中", 99),
                                                         repeat("词", 50), {repeat("义", 500)});
        check(unicodeWord.meanings[0].size() == repeat("义", 500).size(), "长度应按 Unicode 码点而不是字节计算。");
        repository.createWord(privateBook, aliceId, "can't-stop now", "phrase", {"短语"});

        const auto importBook = repository.createWordbook("导入测试", aliceId);
        checkCounts(repository.importTsv(importBook, aliceId,
            "\xef\xbb\xbf" "english\tpos\tmeaning\r\n\r\n"
            " Case \t \t first \r\ncase\tn.\tsecond\r\nCASE\tv.\t second \r\n"
            "Case\tn.\tfirst\r\nnew-word\tadj.\t新\r\nNEW-WORD\tadj.\t新", "merge"), 2, 1, 3);
        auto imported = repository.listWords(importBook, aliceId);
        auto caseWord = findEnglish(imported, "Case");
        check(caseWord.partOfSpeech == "n." && caseWord.meanings.size() == 2,
              "merge 应补空词性/多释义，不覆盖非空词性。");
        checkCounts(repository.importTsv(importBook, aliceId,
            "english\tpos\tmeaning\ncase\tv.\tthird\nthird\t\t三\nTHIRD\tn.\t第三\n", "skip"), 1, 0, 2);
        check(repository.getWord(caseWord.id, aliceId).meanings.size() == 2 &&
                  findEnglish(repository.listWords(importBook, aliceId), "third").meanings.size() == 1,
              "skip 不得补释义或填词性，包括同一批中后续重复行。");
        const auto emptyPos = repository.createWord(importBook, aliceId, "empty-pos", "", {"仅义"});
        checkCounts(repository.importTsv(importBook, aliceId,
            "english\tpos\tmeaning\nEMPTY-POS\tn.\t仅义\nempty-pos\tv.\t仅义\n", "merge"), 0, 1, 1);
        check(repository.getWord(emptyPos.id, aliceId).partOfSpeech == "n.", "仅补空词性也应计 merged。");
        checkCounts(repository.importTsv(importBook, aliceId, "english\tpos\tmeaning\n\n", "merge"), 0, 0, 0);
        expectInvalid([&] { repository.importTsv(importBook, aliceId, "english\tpos\tmeaning", "replace"); });

        const auto beforeInvalid = repository.listWords(importBook, aliceId).size();
        const std::string brokenTsv = "english\tpos\tmeaning\r\n\r\nvalid\tn.\t有效\r\n"
            "bad\tn.\r\n\t \t空英文\r\nbad2\tv.\t \r\nextra\tn.\t义\t额外\r\n"
            "broken\tn.\t" + std::string("\xed\xa0\x80", 3) + "\r\n\t\t\r\n";
        const auto errors = expectImportError([&] { repository.importTsv(importBook, aliceId, brokenTsv, "merge"); });
        for (int line : {4, 5, 6, 7, 8, 9}) {
            check(hasLine(errors, line), "TSV 错误缺少实际输入行 " + std::to_string(line));
        }
        check(!hasLine(errors, 2) && repository.listWords(importBook, aliceId).size() == beforeInvalid,
              "空行应忽略，且存在任一输入错误时有效行也不能写入。");
        for (const auto& bad : badUtf8) {
            check(hasLine(expectImportError([&] {
                repository.importTsv(importBook, aliceId, "english\tpos\tmeaning\n\nutf8\tn.\t" + bad, "merge");
            }), 3), "UTF-8/NUL 错误行号不正确。");
        }
        check(hasLine(expectImportError([&] {
            repository.importTsv(importBook, aliceId, "\n\nEnglish\tpos\tmeaning\nvalid\tn.\t有效", "merge");
        }), 3), "表头错误应保留前导空行后的真实行号。");
        expectImportError([&] { repository.importTsv(importBook, aliceId, "", "merge"); });
        expectImportError([&] { repository.importTsv(importBook, aliceId, std::string(48 * 1024 + 1, 'x'), "merge"); });
        check(hasLine(expectImportError([&] {
            repository.importTsv(importBook, aliceId, "english\tpos\tmeaning\n" + std::string(2000, '\n'), "merge");
        }), 2001), "2000 行上限应包括空行，且错误应指向 2001 行。");
        checkCounts(repository.importTsv(importBook, aliceId,
            "english\tpos\tmeaning\n" + std::string(1998, '\n') + "last-line\t\t最后一行", "merge"), 1, 0, 0);

        std::vector<std::string> fiftyMeanings;
        for (int i = 0; i < 50; ++i) {
            fiftyMeanings.push_back("释义" + std::to_string(i));
        }
        const auto full = repository.createWord(importBook, aliceId, "full-word", "", fiftyMeanings);
        check(repository.addMeaning(full.id, aliceId, " 释义0 ").meanings.size() == 50,
              "达到释义上限后重复释义仍应是无副作用成功。");
        expectInvalid([&] { repository.addMeaning(full.id, aliceId, "第五十一义"); });
        const auto beforeOverflow = repository.listWords(importBook, aliceId).size();
        check(hasLine(expectImportError([&] {
            repository.importTsv(importBook, aliceId,
                "english\tpos\tmeaning\nshould-not-save\t\t回滚\nFULL-WORD\tn.\t第五十一义", "merge");
        }), 3), "合并超过释义上限应返回实际数据行号。");
        check(repository.listWords(importBook, aliceId).size() == beforeOverflow &&
                  repository.getWord(full.id, aliceId).partOfSpeech.empty(), "导入模拟失败不应写入词或词性。");

        // 插入中途发生 SQL 异常：新词、释义和已有词的词性补全必须一起回滚。
        observer->execSqlSync("CREATE TRIGGER reject_test_meaning BEFORE INSERT ON word_meanings "
                              "WHEN NEW.meaning = '触发失败' BEGIN SELECT RAISE(ABORT, 'injected'); END");
        const auto beforeSqlFailure = repository.listWords(importBook, aliceId).size();
        expectFailure([&] {
            repository.importTsv(importBook, aliceId,
                "english\tpos\tmeaning\nthird\tn.\t三\nfirst-ok\t\t成功\nsecond-bad\t\t触发失败", "merge");
        });
        check(repository.listWords(importBook, aliceId).size() == beforeSqlFailure &&
                  findEnglish(repository.listWords(importBook, aliceId), "third").partOfSpeech.empty(),
              "SQL 错误必须回滚全部导入，包括已更新的词性。");
        expectFailure([&] { repository.createWord(importBook, aliceId, "bad-create", "", {"先成功", "触发失败"}); });
        check(repository.listWords(importBook, aliceId).size() == beforeSqlFailure, "新建多释义失败留下了孤立单词。");
        observer->execSqlSync("DROP TRIGGER reject_test_meaning");
        checkCounts(repository.importTsv(importBook, aliceId,
            "english\tpos\tmeaning\nafter-rollback\t\t仍可写入", "merge"), 1, 0, 0);

        // 所有 SQL 均成功、仅 COMMIT 失败的情形，不能错误地返回成功或遗留活动事务。
        observer->execSqlSync("CREATE TABLE commit_probe(id INTEGER PRIMARY KEY, "
                              "missing_user_id INTEGER REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED)");
        observer->execSqlSync("CREATE TRIGGER fail_test_commit AFTER INSERT ON words "
                              "WHEN NEW.english = 'commit-fail' BEGIN "
                              "INSERT INTO commit_probe(id, missing_user_id) VALUES (NEW.id, -999); END");
        const auto beforeCommitFailure = repository.listWords(importBook, aliceId).size();
        expectFailure([&] { repository.createWord(importBook, aliceId, "commit-fail", "", {"提交失败"}); });
        check(repository.listWords(importBook, aliceId).size() == beforeCommitFailure &&
                  observer->execSqlSync("SELECT id FROM commit_probe").empty(),
              "COMMIT 失败未传播或未回滚。");
        expectFailure([&] {
            repository.importTsv(importBook, aliceId,
                "english\tpos\tmeaning\ncommit-before\t\t前行\ncommit-fail\t\t提交失败", "merge");
        });
        check(repository.listWords(importBook, aliceId).size() == beforeCommitFailure,
              "导入提交失败必须整批回滚。");
        observer->execSqlSync("DROP TRIGGER fail_test_commit");
        observer->execSqlSync("DROP TABLE commit_probe");

        const auto deleted = repository.createWord(privateBook, aliceId, "delete-me", "", {"释义甲", "释义乙"});
        observer->execSqlSync("INSERT INTO wrong_words(user_id, word_id) VALUES (?, ?)", aliceId, deleted.id);
        observer->execSqlSync("CREATE TABLE history_probe(id INTEGER PRIMARY KEY, "
                              "word_id INTEGER REFERENCES words(id) ON DELETE SET NULL)");
        observer->execSqlSync("INSERT INTO history_probe(word_id) VALUES (?)", deleted.id);
        repository.deleteWord(deleted.id, aliceId);
        check(observer->execSqlSync("SELECT id FROM word_meanings WHERE word_id = ?", deleted.id).empty() &&
                  observer->execSqlSync("SELECT word_id FROM wrong_words WHERE word_id = ?", deleted.id).empty() &&
                  observer->execSqlSync("SELECT word_id FROM history_probe")[0]["word_id"].isNull(),
              "仓储连接未启用 foreign_keys：删除应级联清理释义/错题并置空历史引用。");
        expectKind(RepositoryError::Kind::NotFound, [&] { repository.getWord(deleted.id, aliceId); });
        check(observer->execSqlSync("PRAGMA foreign_key_check").empty(), "CRUD 后外键不一致。");
        persistedCount = repository.listWords(privateBook, aliceId).size();
    }
    {
        WordRepository restarted(temporary.path.string());
        const auto demos = restarted.listDemoWordbooks();
        check(demos.size() == 1 && demos[0]["wordCount"].asUInt64() == demoCount,
              "重启不能重复导入示例。");
        check(restarted.listWords(privateBook, aliceId).size() == persistedCount &&
                  restarted.getWord(retainedWord, aliceId).meanings.size() == 4,
              "私人词库、修改或补充的多释义未跨重启持久化。");
    }
}

} // namespace

/// 数据库迁移、所有权、统一校验、CRUD、多释义、整批导入和提交确认的独立回归测试。
int main()
{
    try {
        testMigrations();
        testRepository();
        std::cout << "Database migration, permissions, CRUD, TSV transactions and persistence passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
