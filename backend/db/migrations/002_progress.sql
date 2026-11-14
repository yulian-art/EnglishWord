-- 在开启 foreign_keys 的同一个事务中重建，兼容仅接受 choice/spelling 的旧版约束。
-- 新明细先引用新父表，随后先删除旧明细再删除旧父表，避免级联删除历史。
CREATE TABLE study_sessions_v2 (
    id INTEGER PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    wordbook_id INTEGER REFERENCES wordbooks(id) ON DELETE SET NULL,
    wordbook_name TEXT NOT NULL DEFAULT '',
    mode TEXT NOT NULL CHECK (mode IN ('choice', 'spelling', 'meaning')),
    score INTEGER NOT NULL DEFAULT 0,
    total_count INTEGER NOT NULL DEFAULT 0 CHECK (total_count >= 0),
    correct_count INTEGER NOT NULL DEFAULT 0 CHECK (correct_count >= 0),
    pass_threshold REAL NOT NULL DEFAULT 0.8 CHECK (pass_threshold >= 0 AND pass_threshold <= 1),
    started_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    finished_at TEXT
);

INSERT INTO study_sessions_v2
    (id, user_id, wordbook_id, wordbook_name, mode, score, total_count,
     correct_count, pass_threshold, started_at, finished_at)
SELECT s.id, s.user_id, s.wordbook_id, COALESCE(b.name, '已删除词库'), s.mode,
       s.score, s.total_count, s.correct_count, 0.8, s.started_at, s.finished_at
FROM study_sessions s LEFT JOIN wordbooks b ON b.id = s.wordbook_id;

CREATE TABLE answer_records_v2 (
    id INTEGER PRIMARY KEY,
    session_id INTEGER NOT NULL REFERENCES study_sessions_v2(id) ON DELETE CASCADE,
    word_id INTEGER REFERENCES words(id) ON DELETE SET NULL,
    question_index INTEGER CHECK (question_index >= 0),
    submitted_answer TEXT NOT NULL,
    is_correct INTEGER NOT NULL CHECK (is_correct IN (0, 1)),
    is_first_attempt INTEGER NOT NULL CHECK (is_first_attempt IN (0, 1)),
    score_delta INTEGER NOT NULL CHECK (score_delta IN (-10, 0, 10)),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- 旧历史没有题号，不推测或去重，NULL 使其不受新首次提交唯一索引影响。
INSERT INTO answer_records_v2
    (id, session_id, word_id, question_index, submitted_answer, is_correct,
     is_first_attempt, score_delta, created_at)
SELECT id, session_id, word_id, NULL, submitted_answer, is_correct,
       is_first_attempt, score_delta, created_at
FROM answer_records;

DROP TABLE answer_records;
DROP TABLE study_sessions;
ALTER TABLE study_sessions_v2 RENAME TO study_sessions;
ALTER TABLE answer_records_v2 RENAME TO answer_records;

CREATE INDEX idx_sessions_user ON study_sessions(user_id, started_at DESC, id DESC);
CREATE INDEX idx_answers_session ON answer_records(session_id, question_index);
CREATE UNIQUE INDEX idx_answers_first_attempt
    ON answer_records(session_id, question_index)
    WHERE is_first_attempt = 1 AND question_index IS NOT NULL;

-- SQLite 普通 INTEGER PRIMARY KEY 会重用被删除的最大编号。
-- 学习引擎持有单词快照，必须保证删除再新建不会把旧题关联到新词。
-- 002 尚未发布，在本次升级中同时为词条改用 AUTOINCREMENT，并保留所有子记录。
CREATE TABLE words_stable (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    wordbook_id INTEGER NOT NULL REFERENCES wordbooks(id) ON DELETE CASCADE,
    english TEXT NOT NULL COLLATE NOCASE,
    part_of_speech TEXT NOT NULL DEFAULT '',
    UNIQUE (wordbook_id, english)
);
INSERT INTO words_stable SELECT id, wordbook_id, english, part_of_speech FROM words;

CREATE TABLE word_meanings_stable (
    id INTEGER PRIMARY KEY,
    word_id INTEGER NOT NULL REFERENCES words_stable(id) ON DELETE CASCADE,
    meaning TEXT NOT NULL,
    UNIQUE (word_id, meaning)
);
INSERT INTO word_meanings_stable SELECT id, word_id, meaning FROM word_meanings;

CREATE TABLE wrong_words_stable (
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    word_id INTEGER NOT NULL REFERENCES words_stable(id) ON DELETE CASCADE,
    wrong_count INTEGER NOT NULL DEFAULT 1 CHECK (wrong_count >= 1),
    mastered INTEGER NOT NULL DEFAULT 0 CHECK (mastered IN (0, 1)),
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, word_id)
);
INSERT INTO wrong_words_stable SELECT user_id, word_id, wrong_count, mastered, updated_at FROM wrong_words;

CREATE TABLE answer_records_stable (
    id INTEGER PRIMARY KEY,
    session_id INTEGER NOT NULL REFERENCES study_sessions(id) ON DELETE CASCADE,
    word_id INTEGER REFERENCES words_stable(id) ON DELETE SET NULL,
    question_index INTEGER CHECK (question_index >= 0),
    submitted_answer TEXT NOT NULL,
    is_correct INTEGER NOT NULL CHECK (is_correct IN (0, 1)),
    is_first_attempt INTEGER NOT NULL CHECK (is_first_attempt IN (0, 1)),
    score_delta INTEGER NOT NULL CHECK (score_delta IN (-10, 0, 10)),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
INSERT INTO answer_records_stable
    SELECT id, session_id, word_id, question_index, submitted_answer, is_correct,
           is_first_attempt, score_delta, created_at FROM answer_records;

DROP TABLE word_meanings;
DROP TABLE wrong_words;
DROP TABLE answer_records;
DROP TABLE words;
ALTER TABLE words_stable RENAME TO words;
ALTER TABLE word_meanings_stable RENAME TO word_meanings;
ALTER TABLE wrong_words_stable RENAME TO wrong_words;
ALTER TABLE answer_records_stable RENAME TO answer_records;

CREATE INDEX idx_meanings_word ON word_meanings(word_id);
CREATE INDEX idx_answers_session ON answer_records(session_id, question_index);
CREATE UNIQUE INDEX idx_answers_first_attempt
    ON answer_records(session_id, question_index)
    WHERE is_first_attempt = 1 AND question_index IS NOT NULL;
