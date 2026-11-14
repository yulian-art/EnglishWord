# API 规范（后端已实现）

基础地址：`http://127.0.0.1:8080/api`。JSON 字段使用 camelCase，数据库使用 snake_case。成功均返回 200，业务错误统一为 `{"error":"说明"}`；框架产生的 413、路由不存在等错误不保证使用该格式。所有控制器响应均包含 `Cache-Control: no-store`。

## 请求与身份

写请求（POST、PATCH、DELETE）必须设置 `Content-Type: application/json`。除 DELETE 可无请求体外，有体请求均使用 JSON 对象。浏览器使用同源 Cookie；客户端不传 userId、正确答案、分数或打卡日期，后端不信任这些值。请求体上限 64 KiB。

登录和退出会轮换 `JSESSIONID`，轮换前的 Cookie 立即失效；切换账号或退出同时清除临时学习任务。会话闲置一小时或重启后需要重新登录。服务拒绝跨站写请求、非同源 Origin 及简单表单；为兼容当前 Vite 的 `changeOrigin` 代理，本机目标额外信任 `http://127.0.0.1:5173` 和 `http://localhost:5173`，其他开发端口默认不放行。不启用跨域 CORS，也不将身份放入可由前端修改的请求字段。

需要登录的接口未登录返回 401；无权访问其他用户的词库或修改公共示例返回 403；资源不存在返回 404；重名或状态冲突返回 409；字段错误返回 400；数据库等内部故障返回 500，不公开 SQL 详情。匿名用户只能浏览公共词库并进行不持久化的示例学习，不能创建词库或访问历史、错题和打卡。匿名读取私人词库同样返回 403。

## 账号与词库

| 方法 / 路径（省略 /api） | 请求 | 成功返回 |
| --- | --- | --- |
| GET /health | 无 | `{"status":"ok","mode":"demo","framework":"Drogon"}` |
| POST /auth/register | `{"username":"xiaoming","password":"example-password"}` | `{"user":User}`；注册不自动登录 |
| POST /auth/login | 同注册 | `{"user":User}`，设置新登录 Cookie |
| POST /auth/logout | `{}` 或 `{"checkin":true}` | `{"success":true}`；打卡退出另含 `checkin:Checkin` |
| GET /auth/me | 无 | `{"user":User}` |
| GET /wordbooks | 无 | `{"items":Wordbook[]}`，公共加本人私人词库 |
| POST /wordbooks | `{"name":"四级词库"}` | 新建的 Wordbook |
| GET /wordbooks/{id}/words | 词库编号 | `{"items":Word[]}`，空词库为 [] |
| POST /wordbooks/{id}/words | `{"english":"book","partOfSpeech":"n.","meanings":["书"]}` | 新建的 Word |
| PATCH /words/{id} | english、partOfSpeech 至少一个字符串字段 | 更新后的 Word |
| DELETE /words/{id} | 无业务字段 | `{"success":true}` |
| POST /words/{id}/meanings | `{"meaning":"书籍"}` | 补充后的 Word，重复释义不新增 |
| POST /wordbooks/{id}/import | `{"content":"english\tpos\tmeaning\napple\tn.\t苹果","duplicatePolicy":"merge"}` | `{"importedCount":1,"mergedCount":0,"skippedCount":0}` |

User 为 `{id,username,createdAt}`，从不包含密码哈希。用户名是 3～32 位 ASCII 字母、数字或下划线；密码为 8～128 字节，不能包含 NUL。User.createdAt 及其他时间戳为 UTC ISO 8601 字符串，例如 `2026-11-14T10:00:00Z`。

Wordbook 为 `{id,name,wordCount,isDemo}`。Word 为 `{id,english,partOfSpeech,meanings}`，meanings 为字符串数组。词库名 1～50 个 Unicode 字符；英文 1～100 个 Unicode 字符且至少包含一个 ASCII 英文字母；词性 0～50 个字符；每条释义 1～500 个字符，每个词最多 50 条不同释义。文本去除两端 ASCII 空白，拒绝非法 UTF-8、NUL 和内部控制字符。同词库英文忽略 ASCII 大小写去重，冲突返回 409；公共词库只读。

## TSV 导入

content 是 UTF-8 TSV 字符串，不是文件上传；支持 UTF-8 BOM、LF 和 CRLF。表头必须为 `english<TAB>pos<TAB>meaning`，每条数据行恰好三列；pos 可为空，英文和释义不能为空。空白行不导入但计入物理行号。content 最多 48 KiB，最多 2000 行。

duplicatePolicy 仅允许 merge 或 skip。merge 为同词补充新释义，或填充原本为空的词性，不覆盖非空词性；skip 保留已有词。计数按数据行而非最终词数：新词行计 importedCount，同词且新增信息计 mergedCount，跳过或无新增信息计 skippedCount。同一批多行同词可登记多条释义。

格式和字段错误整批拒绝，返回 400：`{"error":"导入失败","errors":[{"line":3,"message":"释义长度必须为 1..500 个 Unicode 字符。"}]}`。line 是包含表头、空行的实际输入行号。校验及数据库写入任一失败均不保留部分导入；只有事务确认提交成功才返回成功。SQL/COMMIT 故障返回 500。

## 背诵接口

| 方法 / 路径 | 请求 | 返回或规则 |
| --- | --- | --- |
| GET /study | 无 | 当前 Cookie、当前身份的 StudyState；无任务 404 |
| POST /study/start | `{"wordbookId":1,"mode":"spelling"}` | 新一轮 StudyState，替换临时任务；旧轮次历史保留 |
| POST /study/answer | `{"answer":"apple"}` | StudyState，另含 result:AnswerResult |
| POST /study/next | `{}` | 当前题答对才前进，完成最后一题时结算 |
| POST /study/review | `{}` | 上轮结束后用其仍存在的错词开始独立拼写轮次 |
| POST /wrong-words/review | `{"wordbookId":2}` | 登录后从所选词库长期未掌握错题开始拼写轮次，不依赖上轮内存 |

mode 为 choice（看中文选英文）、spelling（看中文拼写）或 meaning（看英文写任一登记中文释义）。choice 至少需要两个不同词，最多四个选项。空词库、无错词或非法模式为 400；未答对就 next、未结束就本轮 review 为 409。答案不能是空白、非法 UTF-8 或控制字符，最多 2000 字节；无效输入不消耗首次作答机会。

StudyState 为 `{sessionId,wordbookId,mode,score,correctCount,totalCount,currentIndex,wrongCount,finished,canAdvance,accuracy,passThreshold,passed,question}`。编号和计数为整数；匿名 sessionId 为 null；currentIndex 从 0 开始，完成时等于 totalCount。correctCount 仅计首次答对，wrongCount 是本轮首次答错词数，纠正后仍保留；accuracy 为 correctCount/totalCount。只有完成全部题目且正确率达到本轮阈值，passed 才为 true。

question 在完成后为 null，否则含 `{wordId,partOfSpeech,meanings,choices}`。meaning 模式另含 english:string 且 meanings=[]；其余模式不返回 english。choice 的 choices 是英文文本数组，提交选项文本而非下标；其他模式 choices=[]。题目不暴露独立标准答案字段。

AnswerResult 为 `{correct,firstAttempt,scoreDelta,message}`。首次正确 +10、首次错误 −10、允许负分，即时重试 0 分；同题必须答对才能前进。correct 是本次是否正确；首次错、随后纠正不会增加 correctCount，也不移出本轮错词集合。英文匹配忽略 ASCII 大小写和两端空白，中文匹配任一登记释义，不做语义或模糊匹配。非 meaning 模式答错时提供首字母提示。GET /study 不返回上一次 result。

## 历史、长期错题、统计和打卡

| 方法 / 路径 | 请求 | 成功返回 |
| --- | --- | --- |
| GET /history | 无 | `{"items":HistoryEntry[]}`，本人最近 100 轮，含未完成轮次 |
| GET /statistics | 无 | Statistics，统计本人全部轮次，不受历史 100 条限制 |
| GET /wrong-words | 可选 `?mastered=true` 或 `?mastered=false` | `{"items":[{"word":Word,"wrongCount":2,"mastered":false,"updatedAt":"2026-11-14T10:00:00Z"}]}` |
| POST /checkins | `{}` | `{"date":"2026-11-14","alreadyCheckedIn":false,"totalDays":1}` |
| GET /checkins | 无 | `{"dates":["2026-11-14"],"totalDays":1}`，日期从新到旧 |

HistoryEntry 为 `{id,wordbookId,wordbookName,mode,score,totalCount,correctCount,firstAttemptCount,accuracy,passThreshold,passed,finished,startedAt,finishedAt}`。未完成的 finishedAt 为 null；词库删除后 wordbookId 为 null，但保留名称快照和成绩。每轮阈值在开始时写入数据库，不随配置改变；历史正确率分母为该轮 totalCount。

Statistics 为 `{totalScore,sessionCount,completedSessionCount,firstAttemptCount,correctCount,accuracy,passThreshold,passed}`。sessionCount 包括全部已开始轮次，completedSessionCount 仅计完成轮次；totalScore 包含未完成轮次已提交的分数。累计 accuracy=首次答对数/有效首次作答数，重试不参与分母；没有有效首答时 accuracy=0、passed=false。

wrongCount 是该用户累计首次答错的轮次数，每轮同词最多增加一次。即时重试正确不标记 mastered；后续独立轮次首次答对才标记已掌握，再次首次答错则恢复未掌握。不传 mastered 时返回全部，非法筛选值返回 400。查询和复习始终重新检查当前词库权限；删除词条会清除该词长期错题，但保留历史分数和提交输入，已删词编号不会复用。

Checkin.date 和 dates[] 按 Asia/Shanghai（UTC+8）取服务器当天，不接受客户端指定日期；同一用户同一天多次或多浏览器同时提交只记一天。无需开始学习也可打卡。logout 的 checkin=true 在打卡提交成功后才退出；失败会保留登录状态方便重试。

默认过关阈值是待教师确认的 80%，启动时可用 `--pass-threshold 0.8` 配置，合法范围为 0～1。实现已完成，但不代表教师已确认规则。

## 持久化与验证边界

登录用户的每次有效作答、汇总分数及错题更新同事务提交；数据库失败不推进内存学习状态。重启后能重新登录并读取词库、历史、长期错题和打卡，但不恢复未完成轮次的具体题目位置；匿名示例学习不保存个人历史。v1 数据库自动升级到 v2，原历史保留，运行时无需读取 SQL 或示例文件。

后端测试命令为 `cmake --build --preset dev --parallel 2` 和 `ctest --preset dev`，Windows 多配置环境加 `--config Debug` / `-C Debug`。当前包含 study_engine、database、auth、progress、api_integration 五项。前端页面是否接齐所有新增接口、前端合并冲突和新电脑演示不属于本次后端验证结论。
