#include "controllers/api.h"
#include "services/auth_service.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace englishword {
namespace {

/// 可公开业务错误，由 HTTP 边界保留其状态码与说明。
class ApiError : public std::runtime_error {
public:
    ApiError(drogon::HttpStatusCode status, const std::string& message)
        : std::runtime_error(message), status(status) {}
    drogon::HttpStatusCode status;
};

/// JSON 必须是对象，禁止数组、null 或无法解析的输入进入业务层。
const Json::Value& body(const drogon::HttpRequestPtr& request)
{
    const auto& json = request->getJsonObject();
    if (!json || !json->isObject()) {
        throw std::invalid_argument("请求体必须是 JSON 对象。");
    }
    return *json;
}

Json::Value wordJson(const Word& word)
{
    Json::Value value;
    value["id"] = Json::Int64(word.id);
    value["english"] = word.english;
    value["partOfSpeech"] = word.partOfSpeech;
    value["meanings"] = Json::Value(Json::arrayValue);
    for (const auto& meaning : word.meanings) {
        value["meanings"].append(meaning);
    }
    return value;
}

Json::Value userJson(const User& user)
{
    Json::Value value;
    value["id"] = Json::Int64(user.id);
    value["username"] = user.username;
    value["createdAt"] = user.createdAt;
    return value;
}

std::string modeName(StudyMode mode)
{
    switch (mode) {
    case StudyMode::MultipleChoice: return "choice";
    case StudyMode::Spelling: return "spelling";
    case StudyMode::Meaning: return "meaning";
    }
    throw std::invalid_argument("未知背诵模式。");
}

StudyMode parseMode(const Json::Value& value)
{
    if (value.isString()) {
        if (value.asString() == "choice") return StudyMode::MultipleChoice;
        if (value.asString() == "spelling") return StudyMode::Spelling;
        if (value.asString() == "meaning") return StudyMode::Meaning;
    }
    throw std::invalid_argument("mode 必须是 choice、spelling 或 meaning。");
}

std::int64_t bookId(const Json::Value& json)
{
    if (!json["wordbookId"].isInt64() || json["wordbookId"].asInt64() <= 0) {
        throw std::invalid_argument("wordbookId 必须是正整数。");
    }
    return json["wordbookId"].asInt64();
}

/// 注册和登录共用有界凭据校验，不将密码写日志或响应。
void credentials(const Json::Value& json)
{
    if (!json["username"].isString() || !json["password"].isString()) {
        throw std::invalid_argument("请提供 username 和 password。");
    }
    const auto username = json["username"].asString();
    const auto password = json["password"].asString();
    if (username.size() < 3 || username.size() > 32 ||
        !std::all_of(username.begin(), username.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '_';
        })) {
        throw std::invalid_argument("用户名须为 3 到 32 位字母、数字或下划线。");
    }
    if (password.size() < 8 || password.size() > 128 || password.find('\0') != std::string::npos) {
        throw std::invalid_argument("密码须为 8 到 128 字节，且不能包含 NUL。");
    }
}

/// JSON 同源与 Content-Type 检查  对写请求检查来源，要求 JSON 内容类型
void checkWriteRequest(const drogon::HttpRequestPtr& request)
{
    if (request->method() == drogon::Get || request->method() == drogon::Head) return;
    const auto& site = request->getHeader("sec-fetch-site");
    const auto& origin = request->getHeader("origin");
    const auto& host = request->getHeader("host");
    // Vite 的 changeOrigin 会将 Host 改成后端地址，但保留浏览器 Origin。
    // 只对本机目标放行项目固定的开发源，不接受任意端口、域名或跨站请求。
    const bool localTarget = host.rfind("127.0.0.1:", 0) == 0 || host.rfind("localhost:", 0) == 0;
    const bool viteOrigin = localTarget &&
        (origin == "http://127.0.0.1:5173" || origin == "http://localhost:5173");
    if (site == "cross-site" || (!origin.empty() && !viteOrigin &&
        origin != "http://" + host && origin != "https://" + host)) {
        throw ApiError(drogon::k403Forbidden, "不接受跨源写请求。");
    }
    auto type = request->getHeader("content-type");
    type = type.substr(0, type.find(';'));
    while (!type.empty() && type.back() == ' ') type.pop_back();
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
    });
    if (type != "application/json") {
        throw std::invalid_argument("写请求必须使用 Content-Type: application/json。");
    }
}

/// 限制答案大小并严格检查 UTF-8，避免控制字符、超长输入或无效编码被记为有效首答。
void checkAnswer(const std::string& answer)
{
    if (answer.size() > 2000) throw std::invalid_argument("答案不能超过 2000 字节。");
    for (std::size_t index = 0; index < answer.size();) {
        const auto lead = static_cast<unsigned char>(answer[index++]);
        unsigned value = lead;
        unsigned minimum = 0;
        unsigned count = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { value = lead & 0x1f; count = 1; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { value = lead & 0x0f; count = 2; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { value = lead & 0x07; count = 3; minimum = 0x10000; }
        else if (lead >= 0x80) throw std::invalid_argument("答案必须使用合法 UTF-8。");
        if (count > answer.size() - index) throw std::invalid_argument("答案 UTF-8 不完整。");
        while (count--) {
            const auto next = static_cast<unsigned char>(answer[index++]);
            if ((next & 0xc0) != 0x80) throw std::invalid_argument("答案必须使用合法 UTF-8。");
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            (value < 0x20 && value != '\t' && value != '\r' && value != '\n') ||
            (value >= 0x7f && value <= 0x9f)) {
            throw std::invalid_argument("答案包含非法字符。");
        }
    }
}

Json::Value success()
{
    Json::Value value;
    value["success"] = true;
    return value;
}

} // namespace

Api::Api(WordRepository& repository, UserRepository& users, ProgressRepository& progress,
         double passThreshold)
    : m_repository(repository), m_users(users), m_progress(progress), m_passThreshold(passThreshold)
{
    if (!std::isfinite(passThreshold) || passThreshold < 0.0 || passThreshold > 1.0) {
        throw std::invalid_argument("过关阈值必须在 0 到 1 之间。");
    }
}

void Api::respond(const drogon::HttpRequestPtr& request, const Callback& callback,
                  const std::function<Json::Value()>& action)
{
    Json::Value value;
    auto status = drogon::k200OK;
    try {
        checkWriteRequest(request);
        //同一个 Api 实例的相关业务依次执行，避免并发修改状态
        const std::lock_guard<std::mutex> lock(m_mutex);
        // Drogon 轮换 ID 后会保留旧 ID 别名约十秒；旧 Cookie 不能获得新登录身份。
        // 只拒绝旧请求，不清空共享 Session，避免撤销合法新 Cookie 的身份。
        const auto& cookie = request->getCookie("JSESSIONID");
        if (!cookie.empty() && cookie != request->session()->sessionId()) {
            throw ApiError(drogon::k401Unauthorized, "会话已更新，请使用最新 Cookie 重新登录。");
        }
        // 调用当前路由对应的 action()
        value = action();
    } catch (const ApiError& error) {
        status = error.status;
        value["error"] = error.what();
    } catch (const ImportError& error) {
        status = drogon::k400BadRequest;
        value["error"] = error.what();
        value["errors"] = error.errors;
    } catch (const RepositoryError& error) {
        switch (error.kind) {
        case RepositoryError::Kind::NotFound: status = drogon::k404NotFound; break;
        case RepositoryError::Kind::Forbidden: status = drogon::k403Forbidden; break;
        case RepositoryError::Kind::Conflict: status = drogon::k409Conflict; break;
        }
        value["error"] = error.what();
    } catch (const std::invalid_argument& error) {
        status = drogon::k400BadRequest;
        value["error"] = error.what();
    } catch (const std::logic_error& error) {
        status = drogon::k409Conflict;
        value["error"] = error.what();
    } catch (const std::exception& error) {
        LOG_ERROR << error.what();
        status = drogon::k500InternalServerError;
        value["error"] = "服务器内部错误，请查看后端日志。";
    }
    auto response = drogon::HttpResponse::newHttpJsonResponse(value);
    response->setStatusCode(status);
    response->addHeader("Cache-Control", "no-store");
    callback(response);
}
// 从会话读取 user_id  去用户仓储查账号
User Api::requireUser(const drogon::HttpRequestPtr& request) const
{
    const auto id = request->session()->getOptional<std::int64_t>("user_id");
    if (id) {
        const auto user = m_users.findById(*id);
        if (user) return *user;
        request->session()->erase("user_id");
        request->session()->erase("study");
    }
    throw ApiError(drogon::k401Unauthorized, "请先登录。");
}
// 有登录身份就验证用户并返回编号，否则返回 0。
std::int64_t Api::userId(const drogon::HttpRequestPtr& request) const
{
    return request->session()->getOptional<std::int64_t>("user_id") ? requireUser(request).id : 0;
}

std::shared_ptr<Api::StudyContext> Api::engine(const drogon::HttpRequestPtr& request) const
{
    const auto current = request->session()->getOptional<std::shared_ptr<StudyContext>>("study");
    if (!current || !*current || (*current)->userId != userId(request)) {
        throw ApiError(drogon::k404NotFound, "当前没有背诵任务，请先开始一轮背诵。");
    }
    return *current;
}

void Api::save(const drogon::HttpRequestPtr& request,
               const std::shared_ptr<StudyContext>& context) const
{
    request->session()->modify<std::shared_ptr<StudyContext>>("study",
        [&context](auto& current) { current = context; });
}

Json::Value Api::start(const drogon::HttpRequestPtr& request, std::vector<Word> words,
                       std::int64_t id, StudyMode mode)
{
    if (mode == StudyMode::MultipleChoice && words.size() < 2) {
        throw std::invalid_argument("选择题至少需要两个不同单词。");
    }
    auto context = std::make_shared<StudyContext>();
    context->userId = userId(request);
    context->bookId = id;
    context->passThreshold = m_passThreshold;
    context->study.start(std::move(words), mode);
    if (context->userId) {
        context->sessionId = m_progress.startSession(context->userId, id, modeName(mode),
            context->study.totalCount(), m_passThreshold);
    }
    save(request, context);
    return snapshot(*context);
}

Json::Value Api::snapshot(StudyContext& context) const
{
    auto& study = context.study;
    Json::Value value;
    value["sessionId"] = context.sessionId ? Json::Value(Json::Int64(context.sessionId)) : Json::nullValue;
    value["wordbookId"] = Json::Int64(context.bookId);
    value["mode"] = modeName(study.mode());
    value["score"] = study.score();
    value["correctCount"] = Json::UInt64(study.correctCount());
    value["totalCount"] = Json::UInt64(study.totalCount());
    value["currentIndex"] = Json::UInt64(study.currentIndex());
    value["finished"] = study.finished();
    value["canAdvance"] = study.canAdvance();
    value["wrongCount"] = Json::UInt64(study.wrongWords().size());
    value["accuracy"] = study.totalCount() ? static_cast<double>(study.correctCount()) / study.totalCount() : 0.0;
    value["passThreshold"] = context.passThreshold;
    value["passed"] = study.finished() && study.totalCount() > 0 && value["accuracy"].asDouble() >= context.passThreshold;
    value["question"] = Json::nullValue;
    if (!study.finished()) {
        const auto& word = study.currentWord();
        Json::Value question;
        question["wordId"] = Json::Int64(word.id);
        question["partOfSpeech"] = word.partOfSpeech;
        question["choices"] = Json::Value(Json::arrayValue);
        question["meanings"] = Json::Value(Json::arrayValue);
        if (study.mode() == StudyMode::Meaning) question["english"] = word.english;
        else {
            question["meanings"] = wordJson(word)["meanings"];
            if (study.mode() == StudyMode::MultipleChoice) {
                for (const auto& option : study.choices()) question["choices"].append(option);
            }
        }
        value["question"] = question;
    }
    return value;
}

void Api::registerRoutes(drogon::HttpAppFramework& app)
{
    // 无路径参数与单整数路径参数的适配器，共用验证、锁和错误格式。
    const auto route = [this, &app](const std::string& path, drogon::HttpMethod method,
        const std::function<Json::Value(const drogon::HttpRequestPtr&)>& action) {
        app.registerHandler(path, [this, action](const drogon::HttpRequestPtr& request, Callback&& callback) {
            respond(request, callback, [&] { return action(request); });
        }, {method});
    };
    // 处理带编号的路径，例如 /api/words/{1}。
    const auto withId = [this, &app](const std::string& path, drogon::HttpMethod method,
        const std::function<Json::Value(const drogon::HttpRequestPtr&, std::int64_t)>& action) {
        app.registerHandler(path, [this, action](const drogon::HttpRequestPtr& request,
                                               Callback&& callback, std::int64_t id) {
            respond(request, callback, [&] { return action(request, id); });
        }, {method});
    };

    route("/api/health", drogon::Get, [](const auto&) {
        Json::Value value;
        value["status"] = "ok";
        value["mode"] = "demo";
        value["framework"] = "Drogon";
        return value;
    });
    route("/api/auth/register", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        const auto& json = body(request);
        credentials(json);
        const auto username = json["username"].asString();
        if (m_users.findByUsername(username)) {
            throw ApiError(drogon::k409Conflict, "用户名已存在。");
        }
        std::int64_t id = 0;
        try {
            id = m_users.create(username, AuthService::hashPassword(json["password"].asString()));
        } catch (const drogon::orm::DrogonDbException&) {
            // 外部进程注册同名账号时仍由 UNIQUE 兜底；其他数据库故障不能误报重名。
            if (m_users.findByUsername(username)) throw ApiError(drogon::k409Conflict, "用户名已存在。");
            throw;
        }
        const auto created = m_users.findById(id);
        if (!created) throw std::runtime_error("注册后未能读回用户记录。");
        Json::Value value;
        value["user"] = userJson(*created);
        return value;
    });
    route("/api/auth/login", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        const auto& json = body(request);
        credentials(json);
        const auto user = m_users.findByUsername(json["username"].asString());
        if (!user || !AuthService::verifyPassword(json["password"].asString(), user->passwordHash)) {
            throw ApiError(drogon::k401Unauthorized, "用户名或密码错误。");
        }
        // 切换账号绝不能继承匿名或其他账号的临时题目、轮次和数据库编号。
        request->session()->erase("study");
        request->session()->modify<std::int64_t>("user_id", [&user](auto& id) { id = user->id; });
        request->session()->changeSessionIdToClient();
        Json::Value value;
        value["user"] = userJson(*user);
        return value;
    });
    route("/api/auth/logout", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        const auto& json = body(request);
        if (json.isMember("checkin") && !json["checkin"].isBool()) {
            throw std::invalid_argument("checkin 必须是布尔值。");
        }
        auto value = success();
        if (json.get("checkin", false).asBool()) {
            // 打卡提交成功后才清身份；失败时保留登录，允许用户重试。
            value["checkin"] = m_progress.checkIn(requireUser(request).id);
        }
        request->session()->erase("study");
        request->session()->erase("user_id");
        request->session()->changeSessionIdToClient();
        return value;
    });
    route("/api/auth/me", drogon::Get, [this](const drogon::HttpRequestPtr& request) {
        Json::Value value;
        value["user"] = userJson(requireUser(request));
        return value;
    });
    route("/api/wordbooks", drogon::Get, [this](const drogon::HttpRequestPtr& request) {
        Json::Value value;
        value["items"] = m_repository.listWordbooksForUser(userId(request));
        return value;
    });
    route("/api/wordbooks", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        const auto user = requireUser(request);
        const auto& json = body(request);
        if (!json["name"].isString()) throw std::invalid_argument("请提供词库名称 name。");
        const auto id = m_repository.createWordbook(json["name"].asString(), user.id);
        // 仓储会 trim 并校验 Unicode 长度，因此返回数据库中的规范化名称。
        for (const auto& book : m_repository.listWordbooksForUser(user.id)) {
            if (book["id"].asInt64() == id) return book;
        }
        throw std::runtime_error("创建后未能读回词库。");
    });
    withId("/api/wordbooks/{1}/words", drogon::Get, [this](const drogon::HttpRequestPtr& request, auto id) {
        Json::Value value;
        value["items"] = Json::Value(Json::arrayValue);
        for (const auto& word : m_repository.listWords(id, userId(request))) {
            value["items"].append(wordJson(word));
        }
        return value;
    });
    withId("/api/wordbooks/{1}/words", drogon::Post, [this](const drogon::HttpRequestPtr& request, auto id) {
        const auto user = requireUser(request);
        const auto& json = body(request);
        if (!json["english"].isString() || !json["partOfSpeech"].isString() ||
            !json["meanings"].isArray()) {
            throw std::invalid_argument("请提供 english、partOfSpeech 和 meanings 数组。");
        }
        std::vector<std::string> meanings;
        for (const auto& meaning : json["meanings"]) {
            if (!meaning.isString()) throw std::invalid_argument("meanings 每项必须是字符串。");
            meanings.push_back(meaning.asString());
        }
        return wordJson(m_repository.createWord(id, user.id, json["english"].asString(),
            json["partOfSpeech"].asString(), meanings));
    });
    withId("/api/wordbooks/{1}/import", drogon::Post, [this](const drogon::HttpRequestPtr& request, auto id) {
        const auto user = requireUser(request);
        const auto& json = body(request);
        if (!json["content"].isString() || !json["duplicatePolicy"].isString()) {
            throw std::invalid_argument("请提供 content 和 duplicatePolicy 字符串。");
        }
        return m_repository.importTsv(id, user.id, json["content"].asString(),
            json["duplicatePolicy"].asString());
    });
    withId("/api/words/{1}", drogon::Patch, [this](const drogon::HttpRequestPtr& request, auto id) {
        const auto user = requireUser(request);
        const auto& json = body(request);
        std::optional<std::string> english;
        std::optional<std::string> pos;
        if (json.isMember("english")) {
            if (!json["english"].isString()) throw std::invalid_argument("english 必须是字符串。");
            english = json["english"].asString();
        }
        if (json.isMember("partOfSpeech")) {
            if (!json["partOfSpeech"].isString()) throw std::invalid_argument("partOfSpeech 必须是字符串。");
            pos = json["partOfSpeech"].asString();
        }
        return wordJson(m_repository.updateWord(id, user.id, english, pos));
    });
    withId("/api/words/{1}", drogon::Delete, [this](const drogon::HttpRequestPtr& request, auto id) {
        m_repository.deleteWord(id, requireUser(request).id);
        return success();
    });
    withId("/api/words/{1}/meanings", drogon::Post, [this](const drogon::HttpRequestPtr& request, auto id) {
        const auto user = requireUser(request);
        const auto& json = body(request);
        if (!json["meaning"].isString()) throw std::invalid_argument("meaning 必须是字符串。");
        return wordJson(m_repository.addMeaning(id, user.id, json["meaning"].asString()));
    });
    route("/api/study", drogon::Get, [this](const drogon::HttpRequestPtr& request) {
        return snapshot(*engine(request));
    });
    route("/api/study/start", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        const auto& json = body(request);
        const auto id = bookId(json);
        const auto mode = parseMode(json["mode"]);
        return start(request, m_repository.listWords(id, userId(request)), id, mode);
    });
    route("/api/study/answer", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        const auto& json = body(request);
        if (!json["answer"].isString()) throw std::invalid_argument("answer 必须是字符串。");
        const auto answer = json["answer"].asString();
        checkAnswer(answer);
        // 对副本判题；只有数据库提交成功才替换内存，失败不消耗首次作答机会。
        auto context = std::make_shared<StudyContext>(*engine(request));
        const auto word = context->study.currentWord();
        const auto index = context->study.currentIndex();
        const auto result = context->study.submit(answer);
        if (context->userId) {
            m_progress.recordAnswer(context->userId, context->sessionId, context->bookId, word,
                index, answer, result, context->study.score(), context->study.correctCount());
        }
        save(request, context);
        auto value = snapshot(*context);
        value["result"]["correct"] = result.correct;
        value["result"]["firstAttempt"] = result.firstAttempt;
        value["result"]["scoreDelta"] = result.scoreDelta;
        std::string message = result.correct ? "回答正确，可以进入下一题。" : "回答错误，请重试。";
        if (!result.correct && context->study.mode() != StudyMode::Meaning) {
            message += "首字母提示：" + result.expectedAnswer.substr(0, 1);
        }
        value["result"]["message"] = message;
        return value;
    });
    route("/api/study/next", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        body(request);
        auto context = std::make_shared<StudyContext>(*engine(request));
        context->study.advance();
        if (context->userId && context->study.finished()) {
            m_progress.finishSession(context->userId, context->sessionId,
                context->study.score(), context->study.correctCount());
        }
        save(request, context);
        return snapshot(*context);
    });
    route("/api/study/review", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        body(request);
        const auto previous = engine(request);
        if (!previous->study.finished()) throw std::logic_error("请先完成当前轮次。");
        std::unordered_set<std::int64_t> wrong;
        for (const auto& word : previous->study.wrongWords()) wrong.insert(word.id);
        // 重新取最新词条，已删除的错词不重新关联到历史快照。
        auto words = m_repository.listWords(previous->bookId, userId(request));
        words.erase(std::remove_if(words.begin(), words.end(), [&](const Word& word) {
            return wrong.count(word.id) == 0;
        }), words.end());
        if (words.empty()) throw std::invalid_argument("没有可复习的错题，继续努力！");
        return start(request, std::move(words), previous->bookId, StudyMode::Spelling);
    });
    route("/api/history", drogon::Get, [this](const drogon::HttpRequestPtr& request) {
        Json::Value value;
        value["items"] = m_progress.history(requireUser(request).id);
        return value;
    });
    route("/api/statistics", drogon::Get, [this](const drogon::HttpRequestPtr& request) {
        return m_progress.statistics(requireUser(request).id, m_passThreshold);
    });
    route("/api/wrong-words", drogon::Get, [this](const drogon::HttpRequestPtr& request) {
        const auto user = requireUser(request);
        std::optional<bool> mastered;
        const auto& parameters = request->parameters();
        const auto found = parameters.find("mastered");
        if (found != parameters.end()) {
            if (found->second != "true" && found->second != "false") {
                throw std::invalid_argument("mastered 只能是 true 或 false。");
            }
            mastered = found->second == "true";
        }
        Json::Value value;
        value["items"] = m_progress.wrongWords(user.id, mastered);
        return value;
    });
    route("/api/wrong-words/review", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        const auto user = requireUser(request);
        const auto id = bookId(body(request));
        auto words = m_repository.listWords(id, user.id);
        std::unordered_set<std::int64_t> wrong;
        for (const auto& word : m_progress.reviewWords(user.id)) wrong.insert(word.id);
        words.erase(std::remove_if(words.begin(), words.end(), [&](const Word& word) {
            return wrong.count(word.id) == 0;
        }), words.end());
        if (words.empty()) throw std::invalid_argument("所选词库没有未掌握的错题，继续努力！");
        return start(request, std::move(words), id, StudyMode::Spelling);
    });
    route("/api/checkins", drogon::Get, [this](const drogon::HttpRequestPtr& request) {
        return m_progress.checkins(requireUser(request).id);
    });
    route("/api/checkins", drogon::Post, [this](const drogon::HttpRequestPtr& request) {
        const auto user = requireUser(request);
        body(request);
        return m_progress.checkIn(user.id);
    });
}

} // namespace englishword
