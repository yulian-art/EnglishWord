import assert from "node:assert/strict";
import test from "node:test";
import {
    parseMeanings,
    trimWordText,
    wordKey,
    wordTextError,
} from "../frontend/src/lib/words.ts";
import {
    formatDateTime,
    percentage,
    questionNumber,
    studyProgress,
} from "../frontend/src/lib/study.ts";

test("词库去重与后端一致：只清理 ASCII 空白、折叠 ASCII 大小写", () => {
    assert.equal(trimWordText(" \tApple\r\n"), "Apple");
    assert.equal(wordKey(" APPLE "), wordKey("apple"));
    assert.notEqual(wordKey("Äpple"), wordKey("äpple"));
    assert.equal(trimWordText("\u3000苹果\u3000"), "\u3000苹果\u3000");
});

test("新增单词支持多行释义，清理空行并去重，保留首条拼写和顺序", () => {
    assert.deepEqual(
        parseMeanings(" 苹果\r\n\r\n苹果树\n苹果\n Apple \n apple "),
        ["苹果", "苹果树", "Apple"],
    );
    assert.deepEqual(parseMeanings(" \t\r\n"), []);
});

test("字段校验拒绝空释义、超长及控制字符，允许清空词性", () => {
    assert.equal(wordTextError("  ", "释义", 500), "请输入释义");
    assert.equal(wordTextError("", "词性", 50, false), undefined);
    assert.match(wordTextError("中".repeat(501), "释义", 500), /500/);
    assert.match(wordTextError("两\t列", "释义", 500), /制表符/);
    assert.match(wordTextError("两\n行", "释义", 500), /换行/);
    assert.match(wordTextError("含\u0085控制字符", "释义", 500), /控制字符/);
    assert.equal(wordTextError("😀".repeat(500), "释义", 500), undefined);
});

test("答题进度按已完成题目计算，完成时不会显示超出总数的题号", () => {
    assert.equal(
        studyProgress({
            currentIndex: 0,
            totalCount: 3,
            canAdvance: false,
            finished: false,
        }),
        0,
    );
    assert.equal(
        studyProgress({
            currentIndex: 0,
            totalCount: 3,
            canAdvance: true,
            finished: false,
        }),
        33,
    );
    assert.equal(
        studyProgress({
            currentIndex: 3,
            totalCount: 3,
            canAdvance: false,
            finished: true,
        }),
        100,
    );
    assert.equal(questionNumber({ currentIndex: 3, totalCount: 3 }), 3);
    assert.equal(percentage(0, 0), 0);
    assert.equal(percentage(Number.NaN, 3), 0);
});

test("历史记录按上海时区显示，并容忍缺失或非法日期", () => {
    assert.match(formatDateTime("2026-11-16T18:30:00Z"), /11.17.*02:30/);
    assert.equal(formatDateTime(null), "—");
    assert.equal(formatDateTime("invalid"), "invalid");
});
