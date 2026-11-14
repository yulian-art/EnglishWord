#include "repositories/progress_repository.h"
#include "repositories/repository_error.h"

#include <cmath>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace englishword {
namespace {

using drogon::orm::DbClientPtr;
using TransactionPtr = std::shared_ptr<drogon::orm::Transaction>;

void requireUser(std::int64_t userId)
{
    if (userId <= 0) {
        throw std::invalid_argument("学习进度需要登录用户。");
    }
}

void requireThreshold(double threshold)
{
    if (!std::isfinite(threshold) || threshold < 0.0 || threshold > 1.0) {
        throw std::invalid_argument("过关阈值必须在 0 到 1 之间。");
    }
}

std::int64_t databaseCount(std::size_t count)
{
    if (count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("题目数量过大。");
    }
    return static_cast<std::int64_t>(count);
}

/// Drogon 析构事务只会异步发起提交，必须等提交回调成功才能向调用者报告成功。
template <typename Operation>
auto transact(const DbClientPtr& client, Operation&& operation)
    -> std::invoke_result_t<Operation, const TransactionPtr&>
{
    auto completion = std::make_shared<std::promise<bool>>();
    auto committed = completion->get_future();
    auto transaction = client->newTransaction(
        [completion](bool success) { completion->set_value(success); });
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<Operation, const TransactionPtr&>>) {
            operation(transaction);
            transaction.reset();
            if (!committed.get()) {
                throw std::runtime_error("学习进度事务提交失败。");
            }
        } else {
            auto result = operation(transaction);
            transaction.reset();
            if (!committed.get()) {
                throw std::runtime_error("学习进度事务提交失败。");
            }
            return result;
        }
    } catch (...) {
        if (transaction) {
            transaction->rollback();
        }
        throw;
    }
}

Json::Value wordJson(const drogon::orm::Row& row)
{
    Json::Value word;
    word["id"] = Json::Int64(row["word_id"].as<std::int64_t>());
    word["english"] = row["english"].as<std::string>();
    word["partOfSpeech"] = row["part_of_speech"].as<std::string>();
    word["meanings"] = Json::Value(Json::arrayValue);
    return word;
}

} // namespace

ProgressRepository::ProgressRepository(const std::string& path)
    : m_client(drogon::orm::DbClient::newSqlite3Client("filename=" + path, 1))
{
    if (!m_client) {
        throw std::runtime_error("Drogon 未启用 SQLite 支持。");
    }
    m_client->execSqlSync("PRAGMA foreign_keys = ON");
    m_client->execSqlSync("PRAGMA busy_timeout = 5000");
}

std::int64_t ProgressRepository::startSession(std::int64_t userId, std::int64_t bookId,
                                             const std::string& mode,
                                             std::size_t totalCount,
                                             double passThreshold) const
{
    requireUser(userId);
    requireThreshold(passThreshold);
    const auto count = databaseCount(totalCount);
    if (count == 0 || (mode != "choice" && mode != "spelling" && mode != "meaning")) {
        throw std::invalid_argument("请选择合法模式及非空题目列表。");
    }
    return transact(m_client, [&](const TransactionPtr& transaction) {
        const auto books = transaction->execSqlSync(
            "SELECT b.name, (SELECT COUNT(*) FROM words w WHERE w.wordbook_id = b.id) AS word_count "
            "FROM wordbooks b WHERE b.id = ? AND (b.owner_user_id IS NULL OR b.owner_user_id = ?)",
            bookId, userId);
        if (books.empty()) {
            throw RepositoryError(RepositoryError::Kind::NotFound, "词库不存在或无权访问。");
        }
        if (books[0]["word_count"].as<std::int64_t>() == 0) {
            throw std::invalid_argument("词库为空，无法开始背诵。");
        }
        const auto inserted = transaction->execSqlSync(
            "INSERT INTO study_sessions(user_id, wordbook_id, wordbook_name, mode, total_count, "
            "pass_threshold) VALUES (?, ?, ?, ?, ?, ?)",
            userId, bookId, books[0]["name"].as<std::string>(), mode, count, passThreshold);
        return static_cast<std::int64_t>(inserted.insertId());
    });
}

void ProgressRepository::recordAnswer(std::int64_t userId, std::int64_t sessionId,
                                      std::int64_t bookId, const Word& word,
                                      std::size_t questionIndex, const std::string& answer,
                                      const AnswerResult& result, int score,
                                      std::size_t correctCount) const
{
    requireUser(userId);
    const auto index = databaseCount(questionIndex);
    const auto correct = databaseCount(correctCount);
    const auto expectedDelta = result.firstAttempt ? (result.correct ? 10 : -10) : 0;
    if (answer.find_first_not_of(" \t\r\n") == std::string::npos ||
        result.scoreDelta != expectedDelta) {
        throw std::invalid_argument("答案或计分结果不合法。");
    }
    transact(m_client, [&](const TransactionPtr& transaction) {
        const auto sessions = transaction->execSqlSync(
            "SELECT wordbook_id, total_count, score, correct_count, finished_at "
            "FROM study_sessions WHERE id = ? AND user_id = ?", sessionId, userId);
        if (sessions.empty()) {
            throw RepositoryError(RepositoryError::Kind::NotFound, "学习轮次不存在。");
        }
        const auto session = sessions[0];
        if (!session["finished_at"].isNull()) {
            throw RepositoryError(RepositoryError::Kind::Conflict, "学习轮次已经完成。");
        }
        // 删除词库后 wordbook_id 为 NULL，仍保存旧快照的输入与成绩，但不再关联当前单词。
        if ((!session["wordbook_id"].isNull() &&
             session["wordbook_id"].as<std::int64_t>() != bookId) ||
            index >= session["total_count"].as<std::int64_t>()) {
            throw RepositoryError(RepositoryError::Kind::Conflict, "题目不属于该学习轮次。");
        }
        const auto previous = transaction->execSqlSync(
            "SELECT a.id FROM answer_records a JOIN study_sessions s ON s.id = a.session_id "
            "WHERE s.user_id = ? AND a.session_id = ? AND a.question_index = ? "
            "AND a.is_first_attempt = 1", userId, sessionId, index);
        if (result.firstAttempt == !previous.empty()) {
            throw RepositoryError(RepositoryError::Kind::Conflict,
                                  result.firstAttempt ? "本题首次答案已经保存。" : "本题尚无首次答案。");
        }
        const auto oldScore = session["score"].as<std::int64_t>();
        const auto oldCorrect = session["correct_count"].as<std::int64_t>();
        if (oldScore < std::numeric_limits<int>::min() ||
            oldScore > std::numeric_limits<int>::max() ||
            oldScore + result.scoreDelta != score ||
            oldCorrect + (result.firstAttempt && result.correct ? 1 : 0) != correct ||
            correct > session["total_count"].as<std::int64_t>()) {
            throw RepositoryError(RepositoryError::Kind::Conflict, "轮次汇总与作答明细不一致。");
        }
        // 使用 ID、精确英文、原词库及当前权限共同验证，防止删除、移动或改词后的旧快照污染错题本。
        const auto currentWords = transaction->execSqlSync(
            "SELECT w.id FROM words w JOIN wordbooks b ON b.id = w.wordbook_id "
            "JOIN study_sessions s ON s.wordbook_id = b.id "
            "WHERE s.id = ? AND s.user_id = ? AND w.id = ? AND w.english = ? COLLATE BINARY "
            "AND b.id = ? AND (b.owner_user_id IS NULL OR b.owner_user_id = ?)",
            sessionId, userId, word.id, word.english, bookId, userId);
        const int hasCurrentWord = currentWords.empty() ? 0 : 1;
        transaction->execSqlSync(
            "INSERT INTO answer_records(session_id, word_id, question_index, submitted_answer, "
            "is_correct, is_first_attempt, score_delta) "
            "VALUES (?, (SELECT id FROM words WHERE id = ? AND ? = 1), ?, ?, ?, ?, ?)",
            sessionId, word.id, hasCurrentWord, index, answer, static_cast<int>(result.correct),
            static_cast<int>(result.firstAttempt), result.scoreDelta);
        transaction->execSqlSync(
            "UPDATE study_sessions SET score = ?, correct_count = ? WHERE id = ? AND user_id = ?",
            score, correct, sessionId, userId);
        if (result.firstAttempt && hasCurrentWord) {
            if (!result.correct) {
                // 即使调用方在同一轮重复列出一个词，也只累计一个答错轮次。
                const auto wrongAttempts = transaction->execSqlSync(
                    "SELECT COUNT(*) AS count FROM answer_records a "
                    "JOIN study_sessions s ON s.id = a.session_id WHERE s.user_id = ? "
                    "AND a.session_id = ? AND a.word_id = ? AND a.is_first_attempt = 1 "
                    "AND a.is_correct = 0", userId, sessionId, word.id);
                const int increment = wrongAttempts[0]["count"].as<std::int64_t>() == 1 ? 1 : 0;
                transaction->execSqlSync(
                    "INSERT INTO wrong_words(user_id, word_id) VALUES (?, ?) "
                    "ON CONFLICT(user_id, word_id) DO UPDATE SET wrong_count = wrong_count + ?, "
                    "mastered = 0, updated_at = CURRENT_TIMESTAMP", userId, word.id, increment);
            } else {
                // 只有后续轮首次正确才标掌握，本轮纠错或本轮重复出现的词均不提前掌握。
                transaction->execSqlSync(
                    "UPDATE wrong_words SET mastered = 1, updated_at = CURRENT_TIMESTAMP "
                    "WHERE user_id = ? AND word_id = ? AND NOT EXISTS "
                    "(SELECT 1 FROM answer_records a JOIN study_sessions s ON s.id = a.session_id "
                    "WHERE s.user_id = ? AND a.session_id = ? AND a.word_id = ? "
                    "AND a.is_first_attempt = 1 AND a.is_correct = 0)",
                    userId, word.id, userId, sessionId, word.id);
            }
        }
    });
}

void ProgressRepository::finishSession(std::int64_t userId, std::int64_t sessionId,
                                       int score, std::size_t correctCount) const
{
    requireUser(userId);
    const auto correct = databaseCount(correctCount);
    transact(m_client, [&](const TransactionPtr& transaction) {
        const auto sessions = transaction->execSqlSync(
            "SELECT score, correct_count, total_count, finished_at FROM study_sessions WHERE id = ? AND user_id = ?",
            sessionId, userId);
        if (sessions.empty()) {
            throw RepositoryError(RepositoryError::Kind::NotFound, "学习轮次不存在。");
        }
        if (sessions[0]["score"].as<std::int64_t>() != score ||
            sessions[0]["correct_count"].as<std::int64_t>() != correct) {
            throw RepositoryError(RepositoryError::Kind::Conflict, "完成成绩与已保存的作答不一致。");
        }
        if (sessions[0]["finished_at"].isNull()) {
            // 仓储也守住完成边界：每个题号都必须有首次作答和至少一次正确答案。
            // 不能仅凭八成正确率就把剩余未答题目跳过，或把尚未纠正的题目结算。
            const auto solved = transaction->execSqlSync(
                "SELECT COUNT(*) AS count FROM (SELECT question_index FROM answer_records "
                "WHERE session_id = ? AND question_index IS NOT NULL GROUP BY question_index "
                "HAVING MAX(is_first_attempt) = 1 AND MAX(is_correct) = 1)", sessionId);
            if (solved[0]["count"].as<std::int64_t>() != sessions[0]["total_count"].as<std::int64_t>() ||
                sessions[0]["total_count"].as<std::int64_t>() == 0) {
                throw RepositoryError(RepositoryError::Kind::Conflict, "请先正确完成本轮全部题目。");
            }
            transaction->execSqlSync(
                "UPDATE study_sessions SET finished_at = CURRENT_TIMESTAMP "
                "WHERE id = ? AND user_id = ? AND finished_at IS NULL", sessionId, userId);
        }
    });
}

Json::Value ProgressRepository::history(std::int64_t userId) const
{
    requireUser(userId);
    const auto rows = m_client->execSqlSync(
        "SELECT s.id, s.wordbook_id, COALESCE(NULLIF(s.wordbook_name, ''), b.name, '已删除词库') AS book_name, "
        "s.mode, s.score, s.total_count, s.correct_count, s.pass_threshold, "
        "strftime('%Y-%m-%dT%H:%M:%SZ', s.started_at) AS started_at, "
        "strftime('%Y-%m-%dT%H:%M:%SZ', s.finished_at) AS finished_at, "
        "(SELECT COUNT(*) FROM answer_records a WHERE a.session_id = s.id "
        "AND a.is_first_attempt = 1) AS first_attempt_count "
        "FROM study_sessions s LEFT JOIN wordbooks b ON b.id = s.wordbook_id "
        "WHERE s.user_id = ? ORDER BY s.started_at DESC, s.id DESC LIMIT 100", userId);
    Json::Value entries(Json::arrayValue);
    for (const auto& row : rows) {
        Json::Value entry;
        entry["id"] = Json::Int64(row["id"].as<std::int64_t>());
        entry["wordbookId"] = row["wordbook_id"].isNull()
                                  ? Json::Value(Json::nullValue)
                                  : Json::Value(Json::Int64(row["wordbook_id"].as<std::int64_t>()));
        entry["wordbookName"] = row["book_name"].as<std::string>();
        entry["mode"] = row["mode"].as<std::string>();
        entry["score"] = Json::Int64(row["score"].as<std::int64_t>());
        const auto total = row["total_count"].as<std::int64_t>();
        const auto correct = row["correct_count"].as<std::int64_t>();
        const auto attempts = row["first_attempt_count"].as<std::int64_t>();
        const double accuracy = total > 0 ? static_cast<double>(correct) / total : 0.0;
        const double threshold = row["pass_threshold"].as<double>();
        const bool finished = !row["finished_at"].isNull();
        entry["totalCount"] = Json::Int64(total);
        entry["correctCount"] = Json::Int64(correct);
        entry["firstAttemptCount"] = Json::Int64(attempts);
        entry["accuracy"] = accuracy;
        entry["passThreshold"] = threshold;
        entry["passed"] = finished && attempts > 0 && accuracy >= threshold;
        entry["finished"] = finished;
        entry["startedAt"] = row["started_at"].as<std::string>();
        entry["finishedAt"] = finished ? Json::Value(row["finished_at"].as<std::string>())
                                         : Json::Value(Json::nullValue);
        entries.append(std::move(entry));
    }
    return entries;
}

Json::Value ProgressRepository::statistics(std::int64_t userId, double passThreshold) const
{
    requireUser(userId);
    requireThreshold(passThreshold);
    const auto rows = m_client->execSqlSync(
        "SELECT s.total_score, s.session_count, s.completed_count, a.attempt_count, a.correct_count "
        "FROM (SELECT COALESCE(SUM(score), 0) AS total_score, COUNT(*) AS session_count, "
        "COUNT(finished_at) AS completed_count FROM study_sessions WHERE user_id = ?) s "
        "CROSS JOIN (SELECT COUNT(*) AS attempt_count, "
        "COALESCE(SUM(a.is_correct), 0) AS correct_count FROM answer_records a "
        "JOIN study_sessions s ON s.id = a.session_id "
        "WHERE s.user_id = ? AND a.is_first_attempt = 1) a", userId, userId);
    const auto row = rows[0];
    const auto attempts = row["attempt_count"].as<std::int64_t>();
    const auto correct = row["correct_count"].as<std::int64_t>();
    const double accuracy = attempts > 0 ? static_cast<double>(correct) / attempts : 0.0;
    Json::Value stats;
    stats["totalScore"] = Json::Int64(row["total_score"].as<std::int64_t>());
    stats["sessionCount"] = Json::Int64(row["session_count"].as<std::int64_t>());
    stats["completedSessionCount"] = Json::Int64(row["completed_count"].as<std::int64_t>());
    stats["firstAttemptCount"] = Json::Int64(attempts);
    stats["correctCount"] = Json::Int64(correct);
    stats["accuracy"] = accuracy;
    stats["passThreshold"] = passThreshold;
    stats["passed"] = attempts > 0 && accuracy >= passThreshold;
    return stats;
}

Json::Value ProgressRepository::wrongWords(std::int64_t userId,
                                          const std::optional<bool>& mastered) const
{
    requireUser(userId);
    const int filter = mastered ? (*mastered ? 1 : 0) : -1;
    const auto rows = m_client->execSqlSync(
        "SELECT ww.word_id, ww.wrong_count, ww.mastered, "
        "strftime('%Y-%m-%dT%H:%M:%SZ', ww.updated_at) AS updated_at, "
        "w.english, w.part_of_speech, m.meaning FROM wrong_words ww "
        "JOIN words w ON w.id = ww.word_id JOIN wordbooks b ON b.id = w.wordbook_id "
        "LEFT JOIN word_meanings m ON m.word_id = w.id "
        "WHERE ww.user_id = ? AND (b.owner_user_id IS NULL OR b.owner_user_id = ?) "
        "AND (? = -1 OR ww.mastered = ?) ORDER BY ww.updated_at DESC, ww.word_id, m.id",
        userId, userId, filter, filter);
    Json::Value entries(Json::arrayValue);
    std::optional<std::int64_t> previousWord;
    for (const auto& row : rows) {
        const auto id = row["word_id"].as<std::int64_t>();
        if (!previousWord || *previousWord != id) {
            Json::Value entry;
            entry["word"] = wordJson(row);
            entry["wrongCount"] = Json::Int64(row["wrong_count"].as<std::int64_t>());
            entry["mastered"] = row["mastered"].as<int>() != 0;
            entry["updatedAt"] = row["updated_at"].as<std::string>();
            entries.append(std::move(entry));
            previousWord = id;
        }
        if (!row["meaning"].isNull()) {
            entries[entries.size() - 1]["word"]["meanings"].append(row["meaning"].as<std::string>());
        }
    }
    return entries;
}

std::vector<Word> ProgressRepository::reviewWords(std::int64_t userId) const
{
    const auto entries = wrongWords(userId, false);
    std::vector<Word> words;
    words.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto& value = entry["word"];
        Word word;
        word.id = value["id"].asInt64();
        word.english = value["english"].asString();
        word.partOfSpeech = value["partOfSpeech"].asString();
        for (const auto& meaning : value["meanings"]) {
            word.meanings.push_back(meaning.asString());
        }
        words.push_back(std::move(word));
    }
    return words;
}

Json::Value ProgressRepository::checkIn(std::int64_t userId) const
{
    requireUser(userId);
    return transact(m_client, [&](const TransactionPtr& transaction) {
        const auto date = transaction->execSqlSync("SELECT date('now', '+8 hours') AS date")
                              [0]["date"].as<std::string>();
        const auto inserted = transaction->execSqlSync(
            "INSERT OR IGNORE INTO checkins(user_id, checkin_date) VALUES (?, ?)", userId, date);
        const auto total = transaction->execSqlSync(
            "SELECT COUNT(*) AS count FROM checkins WHERE user_id = ?", userId)
                               [0]["count"].as<std::int64_t>();
        Json::Value checkin;
        checkin["date"] = date;
        checkin["alreadyCheckedIn"] = inserted.affectedRows() == 0;
        checkin["totalDays"] = Json::Int64(total);
        return checkin;
    });
}

Json::Value ProgressRepository::checkins(std::int64_t userId) const
{
    requireUser(userId);
    const auto rows = m_client->execSqlSync(
        "SELECT checkin_date FROM checkins WHERE user_id = ? ORDER BY checkin_date DESC", userId);
    Json::Value result;
    result["dates"] = Json::Value(Json::arrayValue);
    for (const auto& row : rows) {
        result["dates"].append(row["checkin_date"].as<std::string>());
    }
    result["totalDays"] = Json::UInt64(rows.size());
    return result;
}

} // namespace englishword
