#pragma once

#include "core/word.h"

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace englishword {

/// 保存登录用户的学习进度。数据库迁移由 WordRepository 在服务启动时执行。
class ProgressRepository {
public:
    /// 打开已有数据库，并为本连接开启外键和 5 秒忙等待。
    explicit ProgressRepository(const std::string& path);

    /// 校验可访问的非空词库，保存题量、模式及过关阈值快照，返回轮次编号。
    std::int64_t startSession(std::int64_t userId, std::int64_t bookId,
                              const std::string& mode, std::size_t totalCount,
                              double passThreshold) const;
    /// 原子保存一次有效提交、轮次汇总及错题变化。重复首次提交抛 Conflict。
    /// questionIndex 为本轮从零开始的题号，score/correctCount 为引擎提交后的汇总。
    void recordAnswer(std::int64_t userId, std::int64_t sessionId, std::int64_t bookId,
                      const Word& word, std::size_t questionIndex,
                      const std::string& answer, const AnswerResult& result,
                      int score, std::size_t correctCount) const;
    /// 将本人的轮次标为完成，重复调用不改变完成时间或成绩。
    void finishSession(std::int64_t userId, std::int64_t sessionId,
                       int score, std::size_t correctCount) const;

    /// 最近 100 轮历史，正确率按本轮总题数计算，过关使用本轮阈值快照。
    Json::Value history(std::int64_t userId) const;
    /// 累计成绩与首次作答正确率，无首次作答时不算过关。
    Json::Value statistics(std::int64_t userId, double passThreshold) const;
    /// 当前仍可访问的错题，mastered 为空时包括已掌握和未掌握项。
    Json::Value wrongWords(std::int64_t userId,
                          const std::optional<bool>& mastered = std::nullopt) const;
    /// 当前仍可访问且未掌握的错词实体，可直接交给 StudyEngine。
    std::vector<Word> reviewWords(std::int64_t userId) const;
    /// 按 Asia/Shanghai 日期打卡，同一用户同一天只累计一次。
    Json::Value checkIn(std::int64_t userId) const;
    /// 返回本人的打卡日期（从新到旧）及累计天数。
    Json::Value checkins(std::int64_t userId) const;

private:
    drogon::orm::DbClientPtr m_client; ///< 单连接客户端，连接级 PRAGMA 不会分散到不同连接。
};

} // namespace englishword
