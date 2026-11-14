# 数据库速查

SQLite 默认文件为 `build/runtime/englishword.sqlite`，可用 `--database` 指定。启动时在事务内运行内嵌迁移，版本由 `PRAGMA user_version` 管理；已有 v1 数据自动升级到 v2，不需要删除数据库。运行时不依赖项目工作目录中的 SQL 或 TSV 文件。

## 八张表与当前用途

| 表 | 职责 | 关键关系与约束 |
| --- | --- | --- |
| users | 注册、登录账号 | username 唯一，只保存 password_hash，created_at 为 UTC |
| wordbooks | 公共或用户私人词库 | owner_user_id 为 NULL 表示公共只读，否则属于指定用户 |
| words | 单词及词性 | wordbook_id 外键；同词库 english 按 ASCII NOCASE 唯一；id 为 AUTOINCREMENT，不复用删除编号 |
| word_meanings | 一个单词的多条释义 | word_id 外键；(word_id,meaning) 唯一 |
| study_sessions | 每轮成绩与开始/完成状态 | user_id、可空 wordbook_id、wordbook_name 快照、mode、score、total_count、correct_count、pass_threshold、started_at、finished_at |
| answer_records | 每次有效提交及即时重试 | session_id、可空 word_id、question_index、submitted_answer、is_correct、is_first_attempt、score_delta、created_at |
| wrong_words | 用户长期错题与掌握状态 | (user_id,word_id) 联合主键；wrong_count、mastered、updated_at |
| checkins | 按上海日期的用户打卡 | (user_id,checkin_date) 联合主键，保证同日幂等 |

上述八张表均已接入后端。词库、释义、学习成绩、错题与打卡不是前端模拟数据；每次查询和写入均从登录会话识别用户，私人数据需验证所属关系。

## 迁移与事务

`001_init.sql` 保持原结构不变。新增 `002_progress.sql` 兼容旧版只允许 choice/spelling 的轮次约束，保留历史后支持 meaning；同时增加题号、阈值快照、词库名称快照，以及不复用的单词编号。迁移在外键开启的事务内按子表到父表顺序替换，保留多释义、作答、错题和打卡记录，最终检查由测试覆盖。

`answer_records` 的部分唯一索引限制同一轮同一 question_index 最多一条首次提交。旧记录没有可靠题号，迁移后题号为 NULL，不猜测或删除历史。Drogon 事务析构仅发起异步提交，因此仓储等待 COMMIT 结果，确认提交成功才向调用方返回。

TSV 整批导入、单词及多释义创建、一次作答的明细/成绩/错题更新、完成结算和打卡均有事务保护。输入错误、SQL 中途失败和 COMMIT 阶段失败均有回滚测试。所有仓储连接启用 `foreign_keys=ON`（涉及关联写入的连接）及忙等待；删除单词级联删除释义和长期错题，历史明细 word_id 置 NULL，保留提交输入及分数。删除词库后轮次 wordbook_id 置 NULL，保留名称快照。

## API 字段与计算方式

| API 字段 | 来源或规则 |
| --- | --- |
| Wordbook.wordCount | 对该词库 words 计数，不另存冗余数量 |
| Word.meanings | word_meanings 按记录组合 |
| StudyState.score / correctCount | 纯 C++ 引擎判定，登录用户同步保存到 study_sessions |
| firstAttempt / scoreDelta / correct | answer_records 的 is_first_attempt / score_delta / is_correct |
| HistoryEntry.accuracy | 本轮 correct_count / total_count |
| Statistics.accuracy | 全部轮次有效首次正确数 / 全部有效首次作答数，不计重试 |
| wrongCount（本轮） | 内存引擎本轮首次答错的不同词数 |
| wrongCount（长期） | wrong_words.wrong_count，累计首次答错的轮次数 |
| Checkin.date / totalDays | 上海当天 YYYY-MM-DD / 该用户打卡记录数 |

每题首次正确 +10、首次错误 −10，重试为零。即时纠错不标记长期掌握；之后独立轮次首次正确才设 mastered=1，重新首次答错则置回 0。查询错题时还会重新验证词库访问权限，旧题快照不能给后来新建的词增加错题。

数据库布尔值为 0/1，API 为 false/true；时间戳以 UTC 保存并以 ISO 8601 输出。打卡业务日期单独按 UTC+8 计算，不把 UTC 日期或客户端日期当作当天。过关阈值在每轮开始时保存，默认 0.8，可用 `--pass-threshold` 调整，最终规则仍待教师确认。

## 持久化边界与验证

数据库 study_sessions 是学习轮次，不是浏览器 Cookie 会话。登录用户每次有效作答即持久化，重启后重新登录可查询历史、长期错题、词库和打卡；未完成轮次显示 finished=false，不恢复内存中的题目位置。匿名示例任务不写个人学习历史。历史接口仅返回最近 100 轮，统计仍覆盖全部轮次。

`database_tests` 覆盖迁移、词库权限、CRUD、多释义、导入及删除级联；`progress_tests` 覆盖历史、重复首答、掌握状态、打卡、迁移兼容和提交失败；`api_integration.py` 启动真实服务验证隔离、并发及同库重启。完整测试运行方式见 [API.md](API.md) 和 [README](../README.md)。
