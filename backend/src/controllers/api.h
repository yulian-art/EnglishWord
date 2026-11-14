#pragma once

#include "core/study_engine.h"
#include "repositories/progress_repository.h"
#include "repositories/user_repository.h"
#include "repositories/word_repository.h"

#include <drogon/drogon.h>

#include <functional>
#include <memory>
#include <mutex>

namespace englishword {

/// HTTP 边界：验证请求与身份，协调纯学习引擎和持久化仓储，不信任客户端分数。
class Api {
public:
    /// 仓储引用需存活至服务器关闭；passThreshold 为 [0,1] 内的默认过关正确率。
    // 构造函数
    Api(WordRepository& repository, UserRepository& users, ProgressRepository& progress,
        double passThreshold = 0.8);
    /// 注册账号、词库、学习、历史、错题和打卡路由。
    // 成员函数
    void registerRoutes(drogon::HttpAppFramework& app);

private:
    using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

    /// 临时轮次绑定创建时的身份；重启后历史仍在，但不恢复未完成题目的内存状态。
    struct StudyContext {
        StudyEngine study;              ///< 仅后端可修改的判题与计分状态。
        std::int64_t userId = 0;         ///< 匿名示例为 0，不产生持久化成绩。
        std::int64_t sessionId = 0;      ///< 数据库学习轮次编号，不是 Cookie 会话编号。
        std::int64_t bookId = 0;         ///< 本轮所属词库，错题重背沿用来源词库。
        double passThreshold = 0.8;      ///< 本轮开始时锁定阈值，配置变更不篡改历史。
    };

    /// 检查同源 JSON 写请求，串行执行业务，将异常统一映射为 JSON 错误。
    void respond(const drogon::HttpRequestPtr& request, const Callback& callback,
                 const std::function<Json::Value()>& action);
    /// 读取并验证登录用户；没有身份或账号已删除时抛 401。
    User requireUser(const drogon::HttpRequestPtr& request) const;
    /// 匿名返回 0，有登录会话时验证账号仍存在。
    std::int64_t userId(const drogon::HttpRequestPtr& request) const;
    /// 返回仅属于当前身份的临时轮次；无轮次时抛 404。
    std::shared_ptr<StudyContext> engine(const drogon::HttpRequestPtr& request) const;
    /// 先创建数据库轮次，成功后才替换临时状态；返回新轮次快照。
    Json::Value start(const drogon::HttpRequestPtr& request, std::vector<Word> words,
                      std::int64_t bookId, StudyMode mode);
    /// 隐藏标准答案，并给出本轮正确率、阈值及完成后的过关状态。
    Json::Value snapshot(StudyContext& context) const;
    /// 替换会话中的临时状态，仅在数据库成功提交后调用。
    void save(const drogon::HttpRequestPtr& request, const std::shared_ptr<StudyContext>& context) const;

    WordRepository& m_repository;       ///< 词库和导入仓储，非拥有引用。
    UserRepository& m_users;            ///< 账号仓储，非拥有引用。
    ProgressRepository& m_progress;     ///< 历史、错题和打卡仓储，非拥有引用。
    double m_passThreshold;             ///< 新轮次及累计统计使用的默认阈值。
    std::mutex m_mutex;                 ///< 防止同 Cookie 并发提交重复计分或内存/数据库失配。
};

} // namespace englishword
