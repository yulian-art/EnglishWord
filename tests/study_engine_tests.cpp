#include "core/study_engine.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/// condition 不成立时抛出包含 message 的异常；Release 模式也会执行断言。
void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/// 验证 action 抛出 Exception 类型的异常，未抛出时使测试失败。
template <typename Exception, typename Action>
void requireThrows(Action action, const std::string& message)
{
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

/// 返回用于业务规则测试的独立词库。
std::vector<englishword::Word> sampleWords()
{
    return {{1, "apple", "n.", {"苹果"}}, {2, "book", "n.", {"书", "书籍"}},
            {3, "water", "n.", {"水"}}, {4, "learn", "v.", {"学习"}},
            {5, "future", "n.", {"未来"}}};
}

} // namespace

/// 验证计分、重试、错题、状态边界和选择题选项；通过返回零。
int main()
{
    try {
        using namespace englishword;
        StudyEngine study; // 独立于 HTTP 和 SQLite 的业务引擎。
        require(study.finished(), "未开始时没有当前题");
        requireThrows<std::invalid_argument>([&] { study.start({}, StudyMode::Spelling); }, "空词库应被拒绝");
        study.start(sampleWords(), StudyMode::Spelling);
        requireThrows<std::logic_error>([&] { study.advance(); }, "未回答不能前进");
        requireThrows<std::invalid_argument>([&] { study.submit(" \t"); }, "空白答案不能计分");
        require(study.score() == 0, "空白提交保持分数");

        const auto wrong = study.submit("wrong"); // 首次错误作答。
        require(!wrong.correct && wrong.firstAttempt && wrong.scoreDelta == -10, "首次答错扣十分");
        require(study.wrongWords().size() == 1, "首次答错进入错题集合");
        const auto retry = study.submit(" APPLE \r\n"); // 大小写和两端空白不影响英文匹配。
        require(retry.correct && !retry.firstAttempt && retry.scoreDelta == 0, "即时纠错不加分");
        require(study.score() == -10 && study.correctCount() == 0, "纠错不改变首次正确率");
        study.submit("apple");
        require(study.score() == -10 && study.canAdvance(), "重复提交不能刷分");
        study.advance();
        const auto correct = study.submit("book"); // 下一题首次答对。
        require(correct.scoreDelta == 10 && study.score() == 0, "首次答对加十分");
        require(study.correctCount() == 1, "统计首次正确题数");

        study.start(sampleWords(), StudyMode::MultipleChoice);
        const auto options = study.choices(); // 当前题随机选项。
        require(options.size() == 4, "大词库提供四个选项");
        require(std::set<std::string>(options.begin(), options.end()).size() == 4, "选项不重复");
        require(std::find(options.begin(), options.end(), "apple") != options.end(), "选项含正确答案");
        require(study.score() == 0 && study.wrongWords().empty(), "重开一轮重置状态");

//11/11/16:59新增用例
        study.start(sampleWords(), StudyMode::Meaning);

        const auto meaningCorrect = study.submit("苹果");
        require(
            meaningCorrect.correct && meaningCorrect.scoreDelta == 10,
            "输入正确中文释义应加十分");

        study.start({sampleWords()[1]}, StudyMode::Meaning);
        const auto secondMeaning = study.submit("书籍");
        require(secondMeaning.correct, "应匹配任意一条已登记释义");

        study.start(sampleWords(), StudyMode::Meaning);
        const auto wrongMeaning = study.submit("错误答案");
        require(
            !wrongMeaning.correct && wrongMeaning.scoreDelta == -10,
            "中文答错扣十分");

        const auto retryMeaning = study.submit("苹果");
        require(
            retryMeaning.correct && !retryMeaning.firstAttempt && retryMeaning.scoreDelta == 0,
            "中文重试答对不重复计分");




        study.start({sampleWords().front()}, StudyMode::Spelling);
        study.submit("apple");
        require(!study.advance() && study.finished(), "最后一题后结束");
        requireThrows<std::logic_error>([&] { study.submit("apple"); }, "结束后不能继续作答");
        requireThrows<std::invalid_argument>([&] {
            study.start({{1, "apple", "", {}}, {2, "APPLE", "", {}}}, StudyMode::Spelling);
        }, "大小写不同的重复答案应拒绝");
        std::cout << "Study engine rules passed.\n";
    } catch (const std::exception& error) { // 测试失败原因。
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
