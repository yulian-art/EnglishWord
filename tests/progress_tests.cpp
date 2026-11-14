#include "core/study_engine.h"
#include "repositories/progress_repository.h"
#include "repositories/repository_error.h"
#include "repositories/user_repository.h"
#include "repositories/word_repository.h"
#include "resources.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace englishword;
using drogon::orm::DbClientPtr;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void rejects(Function&& operation, const std::string& message)
{
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}

template <typename Function>
void conflicts(Function&& operation)
{
    bool rejected = false;
    try {
        operation();
    } catch (const RepositoryError& error) {
        rejected = error.kind == RepositoryError::Kind::Conflict;
    }
    require(rejected, "Expected a repository conflict.");
}

DbClientPtr connect(const std::filesystem::path& path)
{
    auto client = drogon::orm::DbClient::newSqlite3Client("filename=" + path.string(), 1);
    client->execSqlSync("PRAGMA foreign_keys = ON");
    client->execSqlSync("PRAGMA busy_timeout = 5000");
    return client;
}

Json::Value entryFor(const Json::Value& entries, std::int64_t id)
{
    for (const auto& entry : entries) {
        if (entry["id"].asInt64() == id) {
            return entry;
        }
    }
    throw std::runtime_error("Missing history entry.");
}

bool near(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-9;
}

bool isoUtc(const std::string& value)
{
    return value.size() == 20 && value[10] == 'T' && value.back() == 'Z' &&
           value.find(' ') == std::string::npos;
}

struct Fixture {
    std::int64_t bookId;
    Word word;
};

Fixture privateFixture(const DbClientPtr& client, std::int64_t userId,
                       const std::string& name, const std::string& english)
{
    Fixture fixture;
    fixture.bookId = static_cast<std::int64_t>(client->execSqlSync(
        "INSERT INTO wordbooks(owner_user_id, name) VALUES (?, ?)", userId, name).insertId());
    fixture.word.id = static_cast<std::int64_t>(client->execSqlSync(
        "INSERT INTO words(wordbook_id, english, part_of_speech) VALUES (?, ?, 'n.')",
        fixture.bookId, english).insertId());
    fixture.word.english = english;
    fixture.word.partOfSpeech = "n.";
    fixture.word.meanings = {"测试释义", "另一个释义"};
    for (const auto& meaning : fixture.word.meanings) {
        client->execSqlSync("INSERT INTO word_meanings(word_id, meaning) VALUES (?, ?)",
                            fixture.word.id, meaning);
    }
    return fixture;
}

AnswerResult first(bool correct)
{
    AnswerResult result;
    result.correct = correct;
    result.firstAttempt = true;
    result.scoreDelta = correct ? 10 : -10;
    return result;
}

void runProgress(const std::filesystem::path& path)
{
    std::int64_t alice = 0;
    std::int64_t bob = 0;
    std::int64_t initialSession = 0;
    {
        WordRepository words(path.string());
        UserRepository users(path.string());
        alice = users.create("progress-alice", "test-hash-a");
        bob = users.create("progress-bob", "test-hash-b");
        const auto idleUser = users.create("progress-idle", "test-hash-c");
        auto db = connect(path);
        ProgressRepository progress(path.string());
        const auto books = words.listDemoWordbooks();
        const auto bookId = books[0]["id"].asInt64();
        const auto demo = words.listDemoWords(bookId);
        require(demo.size() >= 2, "Expected demo words.");
        const auto& word = demo[0];
        require(progress.history(alice).empty() && progress.wrongWords(alice).empty() &&
                progress.reviewWords(alice).empty(), "New account must have no progress.");
        const auto empty = progress.statistics(idleUser, 0.0);
        require(empty["sessionCount"].asInt() == 0 && empty["firstAttemptCount"].asInt() == 0 &&
                !empty["passed"].asBool() && near(empty["accuracy"].asDouble(), 0.0),
                "No attempts must never pass, even at a zero threshold.");
        rejects([&] { progress.startSession(alice, bookId, "invalid", 1, 0.8); }, "Invalid mode accepted.");
        rejects([&] { progress.startSession(alice, bookId, "choice", 0, 0.8); }, "Empty round accepted.");
        for (double invalid : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
            rejects([&] { progress.startSession(alice, bookId, "choice", 1, invalid); },
                    "Invalid threshold accepted.");
            rejects([&] { progress.statistics(alice, invalid); }, "Invalid statistics threshold accepted.");
        }
        rejects([&] { progress.history(0); }, "Anonymous progress accepted.");
        const auto other = privateFixture(db, bob, "Bob private", "privateword");
        rejects([&] { progress.startSession(alice, other.bookId, "spelling", 1, 0.8); },
                "A different user's private book was accessible.");
        const auto emptyBook = words.createWordbook("Empty", alice);
        rejects([&] { progress.startSession(alice, emptyBook, "spelling", 1, 0.8); },
                "Empty source book accepted.");
        rejects([&] { progress.startSession(alice, -1, "spelling", 1, 0.8); }, "Missing book accepted.");

        // 与控制器相同，传入当前题的零基题号和引擎提交后的汇总。
        StudyEngine engine;
        engine.start({demo[0], demo[1]}, StudyMode::Spelling);
        initialSession = progress.startSession(alice, bookId, "spelling", engine.totalCount(), 0.5);
        auto answer = engine.submit("definitely-incorrect");
        progress.recordAnswer(alice, initialSession, bookId, engine.currentWord(), engine.currentIndex(),
                              "definitely-incorrect", answer, engine.score(), engine.correctCount());
        conflicts([&] { progress.finishSession(alice, initialSession, engine.score(), engine.correctCount()); });
        answer = engine.submit("still-incorrect");
        progress.recordAnswer(alice, initialSession, bookId, engine.currentWord(), engine.currentIndex(),
                              "still-incorrect", answer, engine.score(), engine.correctCount());
        answer = engine.submit(word.english);
        progress.recordAnswer(alice, initialSession, bookId, engine.currentWord(), engine.currentIndex(),
                              word.english, answer, engine.score(), engine.correctCount());
        auto wrong = progress.wrongWords(alice);
        require(wrong.size() == 1 && wrong[0]["wrongCount"].asInt() == 1 &&
                !wrong[0]["mastered"].asBool() && isoUtc(wrong[0]["updatedAt"].asString()),
                "Immediate retries must neither increment errors nor master a word.");
        require(wrong[0]["word"]["id"].asInt64() == word.id &&
                wrong[0]["word"]["partOfSpeech"].asString() == word.partOfSpeech &&
                wrong[0]["word"]["meanings"].size() == word.meanings.size(),
                "Wrong-word JSON must include the complete Word.");
        conflicts([&] { progress.recordAnswer(alice, initialSession, bookId, word, 0, "duplicate",
                                               first(false), -20, 0); });
        rejects([&] { progress.recordAnswer(bob, initialSession, bookId, word, 0, "other-user",
                                             first(false), -10, 0); }, "Cross-user answer accepted.");
        rejects([&] { progress.finishSession(bob, initialSession, -10, 0); }, "Cross-user finish accepted.");
        require(progress.statistics(alice, 0.8)["totalScore"].asInt() == -10 &&
                progress.statistics(alice, 0.8)["firstAttemptCount"].asInt() == 1 &&
                progress.wrongWords(alice)[0]["wrongCount"].asInt() == 1,
                "Duplicate first attempts changed aggregates.");
        require(db->execSqlSync("SELECT COUNT(*) AS count FROM answer_records WHERE session_id = ?",
                                initialSession)[0]["count"].as<int>() == 3,
                "All retries must have persisted independently.");
        // 数据库唯一索引也须拦截重复，不能仅依赖仓储预查询。
        rejects([&] {
            db->execSqlSync("INSERT INTO answer_records(session_id, question_index, submitted_answer, "
                            "is_correct, is_first_attempt, score_delta) VALUES (?, 0, 'duplicate', 0, 1, -10)",
                            initialSession);
        }, "First-attempt unique index is missing.");
        conflicts([&] { progress.finishSession(alice, initialSession, engine.score(), engine.correctCount()); });
        engine.advance();
        answer = engine.submit(engine.currentWord().english);
        conflicts([&] { progress.recordAnswer(alice, initialSession, bookId, engine.currentWord(), 1,
                                               engine.currentWord().english, answer, 100, 1); });
        progress.recordAnswer(alice, initialSession, bookId, engine.currentWord(), engine.currentIndex(),
                              engine.currentWord().english, answer, engine.score(), engine.correctCount());
        auto history = entryFor(progress.history(alice), initialSession);
        require(!history["finished"].asBool() && !history["passed"].asBool() && history["finishedAt"].isNull() &&
                near(history["accuracy"].asDouble(), 0.5), "Unfinished sessions must not pass.");
        progress.finishSession(alice, initialSession, engine.score(), engine.correctCount());
        const auto completed = entryFor(progress.history(alice), initialSession);
        progress.finishSession(alice, initialSession, engine.score(), engine.correctCount());
        require(entryFor(progress.history(alice), initialSession)["finishedAt"] == completed["finishedAt"] &&
                completed["passed"].asBool() && completed["firstAttemptCount"].asInt() == 2 &&
                completed["correctCount"].asInt() == 1 && isoUtc(completed["startedAt"].asString()) &&
                isoUtc(completed["finishedAt"].asString()), "Finished history or idempotency is incorrect.");
        conflicts([&] { progress.recordAnswer(alice, initialSession, bookId, word, 0,
                                               word.english, first(true), 10, 2); });

        const auto bobSession = progress.startSession(bob, bookId, "choice", 1, 1.0);
        progress.recordAnswer(bob, bobSession, bookId, word, 0, word.english, first(true), 10, 1);
        progress.finishSession(bob, bobSession, 10, 1);
        require(progress.history(bob).size() == 1 && progress.wrongWords(bob).empty() &&
                progress.statistics(bob, 0.8)["totalScore"].asInt() == 10 &&
                progress.statistics(bob, 0.8)["passed"].asBool(), "Per-user history/statistics leaked.");

        const auto masteredSession = progress.startSession(alice, bookId, "meaning", 1, 1.0);
        progress.recordAnswer(alice, masteredSession, bookId, word, 0, word.meanings[0], first(true), 10, 1);
        progress.finishSession(alice, masteredSession, 10, 1);
        require(progress.wrongWords(alice, true).size() == 1 && progress.wrongWords(alice, false).empty() &&
                progress.reviewWords(alice).empty() && progress.wrongWords(alice)[0]["wrongCount"].asInt() == 1,
                "A subsequent round's first correct answer must master the existing wrong word.");
        const auto wrongAgain = progress.startSession(alice, bookId, "spelling", 1, 0.0);
        progress.recordAnswer(alice, wrongAgain, bookId, word, 0, "wrong-again", first(false), -10, 0);
        AnswerResult retry;
        retry.correct = true;
        progress.recordAnswer(alice, wrongAgain, bookId, word, 0, word.english, retry, -10, 0);
        progress.finishSession(alice, wrongAgain, -10, 0);
        require(progress.wrongWords(alice, false).size() == 1 && progress.reviewWords(alice).size() == 1 &&
                progress.wrongWords(alice)[0]["wrongCount"].asInt() == 2 &&
                !progress.wrongWords(alice)[0]["mastered"].asBool(), "Another first error must reset mastery.");
        const auto stats = progress.statistics(alice, 0.9);
        require(stats["totalScore"].asInt() == 0 && stats["sessionCount"].asInt() == 3 &&
                stats["completedSessionCount"].asInt() == 3 && stats["firstAttemptCount"].asInt() == 4 &&
                stats["correctCount"].asInt() == 2 && near(stats["accuracy"].asDouble(), 0.5) &&
                !stats["passed"].asBool(), "Cumulative accuracy must use first attempts, excluding retries.");
        history = progress.history(alice);
        require(near(entryFor(history, initialSession)["passThreshold"].asDouble(), 0.5) &&
                entryFor(history, initialSession)["passed"].asBool() &&
                near(entryFor(history, masteredSession)["passThreshold"].asDouble(), 1.0) &&
                entryFor(history, wrongAgain)["passed"].asBool(), "Historical thresholds must not change with settings.");
        const auto noAttempts = progress.startSession(alice, bookId, "choice", 10, 0.0);
        conflicts([&] { progress.finishSession(alice, noAttempts, 0, 0); });
        require(!entryFor(progress.history(alice), noAttempts)["finished"].asBool() &&
                !entryFor(progress.history(alice), noAttempts)["passed"].asBool() &&
                near(progress.statistics(alice, 0.0)["accuracy"].asDouble(), 0.5),
                "Unanswered rounds must not alter the cumulative denominator or pass by themselves.");

        const auto checked = progress.checkIn(alice);
        const auto checkedAgain = progress.checkIn(alice);
        const auto today = db->execSqlSync("SELECT date('now', '+8 hours') AS date")[0]["date"].as<std::string>();
        require(checked["date"].asString() == today && !checked["alreadyCheckedIn"].asBool() &&
                checkedAgain["alreadyCheckedIn"].asBool() && checkedAgain["totalDays"].asInt() == 1,
                "Shanghai-date check-in must be idempotent.");
        db->execSqlSync("INSERT INTO checkins(user_id, checkin_date) VALUES (?, '2000-01-01')", alice);
        require(progress.checkins(alice)["totalDays"].asInt() == 2 &&
                progress.checkins(alice)["dates"][0].asString() == today &&
                progress.checkins(bob)["dates"].empty(), "Check-in dates/counts leaked between users.");
        require(!progress.checkIn(idleUser)["alreadyCheckedIn"].asBool() &&
                progress.history(idleUser).empty(), "Checking in without studying must be supported.");

        // 已删除、移动、改英文或移交他人的词，只保留作答输入与分数，不污染错题本。
        for (int change = 0; change < 5; ++change) {
            const auto fixture = privateFixture(db, alice, "Snapshot book", "snapshot-" + std::to_string(change));
            const auto session = progress.startSession(alice, fixture.bookId, "spelling", 1, 0.8);
            if (change == 0) {
                db->execSqlSync("DELETE FROM words WHERE id = ?", fixture.word.id);
            } else if (change == 1) {
                db->execSqlSync("UPDATE words SET english = 'renamed-word' WHERE id = ?", fixture.word.id);
            } else if (change == 2) {
                db->execSqlSync("UPDATE words SET wordbook_id = ? WHERE id = ?", other.bookId, fixture.word.id);
            } else if (change == 3) {
                db->execSqlSync("UPDATE wordbooks SET owner_user_id = ? WHERE id = ?", bob, fixture.bookId);
            } else {
                db->execSqlSync("DELETE FROM wordbooks WHERE id = ?", fixture.bookId);
            }
            progress.recordAnswer(alice, session, fixture.bookId, fixture.word, 0, "stale-input", first(false), -10, 0);
            progress.recordAnswer(alice, session, fixture.bookId, fixture.word, 0, fixture.word.english, retry, -10, 0);
            progress.finishSession(alice, session, -10, 0);
            const auto record = db->execSqlSync(
                "SELECT word_id, submitted_answer, score_delta FROM answer_records WHERE session_id = ?", session)[0];
            require(record["word_id"].isNull() && record["submitted_answer"].as<std::string>() == "stale-input" &&
                    record["score_delta"].as<int>() == -10 && progress.wrongWords(alice).size() == 1,
                    "A stale word snapshot linked to a different current word or created a wrong-word row.");
            if (change == 4) {
                const auto deleted = entryFor(progress.history(alice), session);
                require(deleted["wordbookId"].isNull() && deleted["wordbookName"].asString() == "Snapshot book",
                        "Deleted books must preserve session name snapshots and nullable book IDs.");
            }
        }
        const auto owned = privateFixture(db, alice, "Owned book", "accessible-word");
        const auto ownedSession = progress.startSession(alice, owned.bookId, "choice", 1, 0.8);
        progress.recordAnswer(alice, ownedSession, owned.bookId, owned.word, 0, "wrong", first(false), -10, 0);
        require(progress.wrongWords(alice).size() == 2 && progress.reviewWords(alice).size() == 2,
                "Own private wrong words should be accessible.");
        db->execSqlSync("UPDATE wordbooks SET name = 'Changed name', owner_user_id = ? WHERE id = ?", bob, owned.bookId);
        require(progress.wrongWords(alice).size() == 1 && progress.reviewWords(alice).size() == 1 &&
                progress.wrongWords(bob).empty() &&
                entryFor(progress.history(alice), ownedSession)["wordbookName"].asString() == "Owned book",
                "Review queries must recheck access; historical names must remain snapshots.");
        db->execSqlSync("DELETE FROM words WHERE id = ?", owned.word.id);
        require(db->execSqlSync("SELECT word_id FROM answer_records WHERE session_id = ?", ownedSession)[0]["word_id"].isNull(),
                "Foreign keys must preserve submitted answers when words are deleted.");

        // 在错题更新阶段制造失败，前面的明细插入和轮次汇总必须一起回滚。
        const auto atomicSession = progress.startSession(alice, bookId, "choice", 1, 0.8);
        db->execSqlSync("CREATE TRIGGER reject_wrong_insert BEFORE INSERT ON wrong_words "
                        "BEGIN SELECT RAISE(ABORT, 'injected wrong-word failure'); END");
        rejects([&] { progress.recordAnswer(alice, atomicSession, bookId, demo[1], 0,
                                             "injected", first(false), -10, 0); }, "Injected write failure was ignored.");
        db->execSqlSync("DROP TRIGGER reject_wrong_insert");
        require(db->execSqlSync("SELECT COUNT(*) AS count FROM answer_records WHERE session_id = ?", atomicSession)
                    [0]["count"].as<int>() == 0 &&
                entryFor(progress.history(alice), atomicSession)["score"].asInt() == 0,
                "Answer, aggregate and wrong-word changes must roll back together.");
        // 零次尝试后直接传入重试也必须拒绝，不能绕过首次作答唯一性。
        conflicts([&] { progress.recordAnswer(alice, atomicSession, bookId, demo[1], 0,
                                               demo[1].english, retry, 0, 0); });
        require(db->execSqlSync("PRAGMA foreign_key_check").empty(), "Foreign-key integrity was lost.");
    }
    {
        WordRepository reopenedWords(path.string());
        ProgressRepository reopened(path.string());
        require(reopened.history(alice).size() == 11 && reopened.history(bob).size() == 1 &&
                reopened.wrongWords(alice).size() == 1 && reopened.wrongWords(alice)[0]["wrongCount"].asInt() == 2 &&
                reopened.checkins(alice)["totalDays"].asInt() == 2 &&
                entryFor(reopened.history(alice), initialSession)["passed"].asBool(),
                "History, wrong words and check-ins must survive reopening.");
        UserRepository users(path.string());
        const auto limitUser = users.create("history-limit", "test-hash-limit");
        const auto bookId = reopenedWords.listDemoWordbooks()[0]["id"].asInt64();
        std::vector<std::int64_t> sessions;
        for (int index = 0; index < 105; ++index) {
            sessions.push_back(reopened.startSession(limitUser, bookId, "choice", 1, 0.0));
        }
        const auto latest = reopened.history(limitUser);
        require(latest.size() == 100 && latest[0]["id"].asInt64() == sessions.back() &&
                latest[99]["id"].asInt64() == sessions[5] &&
                reopened.statistics(limitUser, 0.0)["sessionCount"].asInt() == 105 &&
                !reopened.statistics(limitUser, 0.0)["passed"].asBool(),
                "History must contain the latest 100 while statistics count every started round.");
    }
}

void runCommitFailures(const std::filesystem::path& path)
{
    WordRepository words(path.string());
    UserRepository users(path.string());
    const auto userId = users.create("commit-failure", "test-hash");
    auto db = connect(path);
    ProgressRepository progress(path.string());
    const auto bookId = words.listDemoWordbooks()[0]["id"].asInt64();
    const auto word = words.listDemoWords(bookId)[0];
    // 延迟外键仅在 COMMIT 时失败，可区分真正等待提交和仅析构事务就报告成功。
    db->execSqlSync("CREATE TABLE commit_guard(user_id INTEGER REFERENCES users(id) "
                    "DEFERRABLE INITIALLY DEFERRED)");
    db->execSqlSync("CREATE TRIGGER fail_start_commit AFTER INSERT ON study_sessions "
                    "BEGIN INSERT INTO commit_guard(user_id) VALUES (-1); END");
    rejects([&] { progress.startSession(userId, bookId, "choice", 1, 0.8); },
            "startSession returned before a failed commit.");
    require(db->execSqlSync("SELECT COUNT(*) AS count FROM study_sessions")[0]["count"].as<int>() == 0,
            "A failed session commit left partial data.");
    db->execSqlSync("DROP TRIGGER fail_start_commit");
    const auto session = progress.startSession(userId, bookId, "choice", 1, 0.8);
    db->execSqlSync("CREATE TRIGGER fail_answer_commit AFTER INSERT ON answer_records "
                    "BEGIN INSERT INTO commit_guard(user_id) VALUES (-1); END");
    rejects([&] { progress.recordAnswer(userId, session, bookId, word, 0, "wrong", first(false), -10, 0); },
            "recordAnswer returned before a failed commit.");
    require(db->execSqlSync("SELECT COUNT(*) AS count FROM answer_records")[0]["count"].as<int>() == 0 &&
            progress.history(userId)[0]["score"].asInt() == 0 && progress.wrongWords(userId).empty(),
            "A failed answer commit left a record, score or wrong word.");
    db->execSqlSync("DROP TRIGGER fail_answer_commit");
    progress.recordAnswer(userId, session, bookId, word, 0, "wrong", first(false), -10, 0);
    AnswerResult retry;
    retry.correct = true;
    progress.recordAnswer(userId, session, bookId, word, 0, word.english, retry, -10, 0);
    db->execSqlSync("CREATE TRIGGER fail_finish_commit AFTER UPDATE OF finished_at ON study_sessions "
                    "BEGIN INSERT INTO commit_guard(user_id) VALUES (-1); END");
    rejects([&] { progress.finishSession(userId, session, -10, 0); },
            "finishSession returned before a failed commit.");
    require(!progress.history(userId)[0]["finished"].asBool(), "A failed finish commit changed completion state.");
    db->execSqlSync("DROP TRIGGER fail_finish_commit");
    progress.finishSession(userId, session, -10, 0);
    db->execSqlSync("CREATE TRIGGER fail_checkin_commit AFTER INSERT ON checkins "
                    "BEGIN INSERT INTO commit_guard(user_id) VALUES (-1); END");
    rejects([&] { progress.checkIn(userId); }, "checkIn returned before a failed commit.");
    require(progress.checkins(userId)["totalDays"].asInt() == 0, "A failed check-in commit was counted.");
    db->execSqlSync("DROP TRIGGER fail_checkin_commit");
    require(!progress.checkIn(userId)["alreadyCheckedIn"].asBool() &&
            db->execSqlSync("SELECT COUNT(*) AS count FROM commit_guard")[0]["count"].as<int>() == 0 &&
            db->execSqlSync("PRAGMA foreign_key_check").empty(), "Commit failures must leave the connection reusable.");
}

void runLegacyMigration(const std::filesystem::path& path)
{
    std::int64_t userId = 0;
    {
        auto db = connect(path);
        std::string schema = resources::schema;
        const std::string current = "mode IN ('choice', 'spelling', 'meaning')";
        const auto position = schema.find(current);
        require(position != std::string::npos, "Cannot construct the legacy mode constraint.");
        schema.replace(position, current.size(), "mode IN ('choice', 'spelling')");
        auto completion = std::make_shared<std::promise<bool>>();
        auto committed = completion->get_future();
        auto transaction = db->newTransaction([completion](bool success) { completion->set_value(success); });
        std::istringstream source(schema);
        std::string statement;
        while (std::getline(source, statement, ';')) {
            if (statement.find_first_not_of(" \t\r\n") != std::string::npos) {
                transaction->execSqlSync(statement);
            }
        }
        transaction->execSqlSync("PRAGMA user_version = 1");
        transaction.reset();
        require(committed.get(), "Legacy fixture commit failed.");
        UserRepository users(path.string());
        userId = users.create("legacy-user", "legacy-hash");
        db->execSqlSync("INSERT INTO wordbooks(id, name) VALUES (31, 'Legacy book')");
        db->execSqlSync("INSERT INTO words(id, wordbook_id, english, part_of_speech) VALUES (41, 31, 'legacy', 'n.')");
        db->execSqlSync("INSERT INTO word_meanings(word_id, meaning) VALUES (41, '旧版')");
        db->execSqlSync("INSERT INTO study_sessions(id, user_id, wordbook_id, mode, score, total_count, "
                        "correct_count, started_at, finished_at) "
                        "VALUES (51, ?, 31, 'spelling', 0, 2, 1, '2024-01-02 03:04:05', '2024-01-02 03:05:06')", userId);
        db->execSqlSync("INSERT INTO answer_records(id, session_id, word_id, submitted_answer, is_correct, "
                        "is_first_attempt, score_delta, created_at) VALUES "
                        "(61, 51, 41, 'wrong', 0, 1, -10, '2024-01-02 03:04:05'), "
                        "(62, 51, 41, 'legacy', 1, 0, 0, '2024-01-02 03:04:15'), "
                        "(63, 51, 41, 'legacy', 1, 1, 10, '2024-01-02 03:04:25')");
        db->execSqlSync("INSERT INTO wrong_words(user_id, word_id, wrong_count) VALUES (?, 41, 3)", userId);
        db->execSqlSync("INSERT INTO checkins(user_id, checkin_date) VALUES (?, '2024-01-02')", userId);
    }
    {
        WordRepository migrated(path.string());
        ProgressRepository progress(path.string());
        auto db = connect(path);
        require(db->execSqlSync("PRAGMA user_version")[0][0].as<int>() == 2 &&
                db->execSqlSync("PRAGMA foreign_keys")[0][0].as<int>() == 1 &&
                db->execSqlSync("PRAGMA foreign_key_check").empty(), "Version 1 migration failed.");
        const auto oldAnswers = db->execSqlSync("SELECT id, session_id, question_index, created_at FROM answer_records ORDER BY id");
        require(oldAnswers.size() == 3 && oldAnswers[0]["id"].as<int>() == 61 &&
                oldAnswers[2]["id"].as<int>() == 63 && oldAnswers[0]["session_id"].as<int>() == 51 &&
                oldAnswers[0]["created_at"].as<std::string>() == "2024-01-02 03:04:05",
                "Rebuilding the parent table cascaded away or changed historical answers.");
        for (const auto& answer : oldAnswers) {
            require(answer["question_index"].isNull(), "Legacy question indexes must remain nullable.");
        }
        auto history = entryFor(progress.history(userId), 51);
        require(history["wordbookName"].asString() == "Legacy book" && history["firstAttemptCount"].asInt() == 2 &&
                history["startedAt"].asString() == "2024-01-02T03:04:05Z" &&
                history["finishedAt"].asString() == "2024-01-02T03:05:06Z" &&
                near(history["passThreshold"].asDouble(), 0.8) && !history["passed"].asBool() &&
                progress.wrongWords(userId)[0]["wrongCount"].asInt() == 3 &&
                progress.checkins(userId)["totalDays"].asInt() == 1,
                "Migration must preserve legacy history, errors and check-ins.");
        const auto word = migrated.listDemoWords(31)[0];
        const auto session = progress.startSession(userId, 31, "meaning", 1, 0.75);
        progress.recordAnswer(userId, session, 31, word, 0, "旧版", first(true), 10, 1);
        progress.finishSession(userId, session, 10, 1);
        require(progress.wrongWords(userId, true).size() == 1,
                "Migrated databases must accept meaning mode and mastery updates.");
        db->execSqlSync("DELETE FROM wordbooks WHERE id = 31");
        require(db->execSqlSync("SELECT COUNT(*) AS count FROM answer_records")[0]["count"].as<int>() == 4 &&
                db->execSqlSync("SELECT COUNT(*) AS count FROM answer_records WHERE word_id IS NULL")[0]["count"].as<int>() == 4 &&
                progress.wrongWords(userId).empty() && entryFor(progress.history(userId), 51)["wordbookId"].isNull() &&
                entryFor(progress.history(userId), 51)["wordbookName"].asString() == "Legacy book" &&
                db->execSqlSync("PRAGMA foreign_key_check").empty(), "Migrated foreign-key actions are incorrect.");
    }
    {
        WordRepository reopened(path.string());
        ProgressRepository progress(path.string());
        require(progress.history(userId).size() == 2 && progress.checkins(userId)["totalDays"].asInt() == 1,
                "Version 2 migration must be restart-idempotent.");
    }
}

} // namespace

int main()
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("englishword-progress-" + std::to_string(unique));
    try {
        std::filesystem::create_directories(directory);
        runProgress(directory / "progress.sqlite");
        runCommitFailures(directory / "commit-failure.sqlite");
        runLegacyMigration(directory / "legacy.sqlite");
        std::filesystem::remove_all(directory);
        std::cout << "Progress persistence, isolation, scoring, wrong words, check-ins and migration passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
