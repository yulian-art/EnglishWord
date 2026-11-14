#include "core/study_engine.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace englishword
{
    namespace
    {

        /// 去除英文答案两端的 ASCII 空白并忽略 ASCII 大小写，返回规范化答案。
        std::string normalize(const std::string &value)
        {
            const auto first = value.find_first_not_of(" \t\r\n"); // 首个非空白字符。
            if (first == std::string::npos)
            {
                return {};
            }
            const auto last = value.find_last_not_of(" \t\r\n");     // 最后一个非空白字符。
            auto normalized = value.substr(first, last - first + 1); // 去掉两端空白的答案。
            for (auto &character : normalized)
            { // 当前英文字符，仅处理 ASCII 字母。
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            return normalized;
        }

    } // namespace

    StudyEngine::StudyEngine() : m_random(std::random_device{}()) {}

    void StudyEngine::start(std::vector<Word> words, StudyMode mode)
    {
        if (words.empty())
        {
            throw std::invalid_argument("词库为空，无法开始背诵。");
        }
        std::unordered_set<std::string> answers; // 去重后的规范英文答案。
        for (const auto &word : words)
        {                                                // 待验证的单词。
            const auto answer = normalize(word.english); // 用于比较的答案。
            if (answer.empty() || !answers.insert(answer).second)
            {
                throw std::invalid_argument("词库含空白或重复英文单词。");
            }
            /*if(mode==StudyMode::Meaning)
            {
                if(word.meanings.empty())
                    throw std::invalid_argument("词库中单词不全有释义");
            }*/
            // 应当进行规范化
            if (mode == StudyMode::Meaning)
            {
                const bool existMeaning = std::any_of(
                    word.meanings.begin(),
                    word.meanings.end(),
                    [](const std::string &meaning)
                    {
                        return !normalize(meaning).empty();
                    });
                if (!existMeaning)
                {
                    throw std::invalid_argument(
                        "词库中单词不全有中文释义,无法开始英文输入中文模式");
                }
            }
        }
        m_words = std::move(words);
        m_mode = mode;
        m_wrongWords.clear();
        m_index = 0;
        m_correctCount = 0;
        m_score = 0;
        m_attempted = false;
        m_solved = false;
    }

    const Word &StudyEngine::currentWord() const
    {
        if (finished())
        {
            throw std::logic_error("当前没有待作答的题目。");
        }
        return m_words[m_index];
    }

    std::vector<std::string> StudyEngine::choices()
    {
        std::vector<std::string> options;    // 先收集干扰项，最后加入正确答案。
        const auto &current = currentWord(); // 当前题目的正确单词。
        for (const auto &word : m_words)
        { // 可用作选项的单词。
            if (normalize(word.english) != normalize(current.english))
            {
                options.push_back(word.english);
            }
        }
        std::shuffle(options.begin(), options.end(), m_random);
        if (options.size() > 3)
        {
            options.resize(3);
        }
        options.push_back(current.english);
        std::shuffle(options.begin(), options.end(), m_random);
        return options;
    }

    AnswerResult StudyEngine::submit(const std::string &answer)
    {
        const auto &word = currentWord();          // 当前待判断的单词。
        const auto normalized = normalize(answer); // 已清理的(规范化的)用户输入。
        if (normalized.empty())
        {
            throw std::invalid_argument("请先输入或选择答案。");
        }
        AnswerResult result; // 此次提交结果。
        result.firstAttempt = !m_attempted;

        if (m_mode == StudyMode::Meaning)
        {
            result.correct=std::any_of(
                word.meanings.begin(),
                word.meanings.end(),
                [normalized](const std::string& meaning){
                    return normalize(meaning)==normalized;
                }
            ); 
            //为了防止输出的正确答案和用户的正确答案不一致:若正确,则输出正确的答案;若错误,则输出第一个中文含义
            result.expectedAnswer=(result.correct)?normalized:*word.meanings.begin();//已经过检查,vector数组非空
        }
        else
        {
            result.correct = normalized == normalize(word.english);
            result.expectedAnswer = word.english;
        }
            // 每题仅首次提交计分；即时纠错不重复加分或扣分。
            if (result.firstAttempt)
            {
                result.scoreDelta = result.correct ? 10 : -10;
                m_score += result.scoreDelta;
                if (result.correct)
                {
                    ++m_correctCount;
                }
                else
                {
                    m_wrongWords.push_back(word);
                }
            }
        
        m_attempted = true;
        m_solved = m_solved || result.correct;
        return result;
    }

    bool StudyEngine::advance()
    {
        if (finished() || !m_solved)
        {
            throw std::logic_error("请先正确回答当前题目。");
        }
        ++m_index;
        m_attempted = false;
        m_solved = false;
        return !finished();
    }

    int StudyEngine::score() const noexcept { return m_score; }
    std::size_t StudyEngine::correctCount() const noexcept { return m_correctCount; }
    std::size_t StudyEngine::totalCount() const noexcept { return m_words.size(); }
    std::size_t StudyEngine::currentIndex() const noexcept { return m_index; }
    bool StudyEngine::finished() const noexcept { return m_index >= m_words.size(); }
    bool StudyEngine::canAdvance() const noexcept { return !finished() && m_solved; }
    StudyMode StudyEngine::mode() const noexcept { return m_mode; }
    const std::vector<Word> &StudyEngine::wrongWords() const noexcept { return m_wrongWords; }

} // namespace englishword

/*
cmake --build --preset core-only --config Debug --parallel 2
ctest --preset core-only -C Debug --output-on-failure
*/