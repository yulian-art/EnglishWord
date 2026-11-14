"""真实子进程 + 独立 SQLite 的 HTTP 回归；仅访问本机，不依赖第三方 Python 包。"""

from concurrent.futures import ThreadPoolExecutor
import datetime
import http.cookiejar
import json
import math
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request


PASSWORDS = {"Alice": "Alice-local-test-42", "Bob": "Bob-local-test-73"}
WRONG_ANSWER = "definitely-not-a-vocabulary-answer"


def expect(actual, expected, label):
    assert actual == expected, f"{label}: expected {expected!r}, got {actual!r}"


def near(actual, expected, label):
    assert math.isclose(actual, expected, abs_tol=1e-12), (
        f"{label}: expected {expected!r}, got {actual!r}"
    )


class Client:
    def __init__(self, server, cookie=None):
        self.server = server
        self.cookie = cookie
        self.jar = http.cookiejar.CookieJar()
        # 禁用环境代理；即使宿主设置了代理，测试也只能直连 127.0.0.1。
        handlers = [urllib.request.ProxyHandler({})]
        if cookie is None:
            handlers.append(urllib.request.HTTPCookieProcessor(self.jar))
        self.opener = urllib.request.build_opener(*handlers)

    def cookie_header(self):
        if self.cookie is not None:
            return self.cookie
        request = urllib.request.Request(self.server.base + "/api/auth/me")
        self.jar.add_cookie_header(request)
        return request.get_header("Cookie", "")

    def request(self, path, data=None, status=200, method=None, headers=None,
                raw=None, json_response=True, timeout=6):
        if raw is None and data is not None:
            raw = json.dumps(data, ensure_ascii=False).encode("utf-8")
        method = method or ("POST" if raw is not None else "GET")
        request_headers = {"Content-Type": "application/json"}
        if self.cookie is not None:
            request_headers["Cookie"] = self.cookie
        request_headers.update(headers or {})
        req = urllib.request.Request(
            self.server.base + path, data=raw, method=method, headers=request_headers
        )
        try:
            response = self.opener.open(req, timeout=timeout)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            payload = response.read()
            text = payload.decode("utf-8", errors="replace")
            expected = status if isinstance(status, tuple) else (status,)
            assert response.code in expected, (
                f"{method} {path}: expected HTTP {expected}, got {response.code}: {text[:1200]}"
            )
            for password in PASSWORDS.values():
                assert password not in text, f"{path}: password leaked in response"
            if not json_response:
                return payload
            try:
                value = json.loads(text)
            except ValueError as error:
                raise AssertionError(f"{method} {path}: non-JSON response: {text[:1200]}") from error
            if response.code >= 400:
                assert isinstance(value.get("error"), str) and value["error"], (path, value)
            expect(response.headers.get("Cache-Control"), "no-store", path + " cache policy")
            return value


class Server:
    def __init__(self, binary, database, threshold=None):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.base = f"http://127.0.0.1:{self.port}"
        self.binary = binary
        self.database = database
        self.threshold = threshold
        self.process = None
        self.logs = tempfile.TemporaryFile()

    def start(self):
        args = [self.binary, "--host", "127.0.0.1", "--port", str(self.port),
                "--database", str(self.database)]
        if self.threshold is not None:
            args += ["--pass-threshold", str(self.threshold)]
        self.logs.write(b"\n--- server start ---\n")
        self.logs.flush()
        self.process = subprocess.Popen(
            args, stdout=self.logs, stderr=self.logs, cwd=str(self.database.parent)
        )
        probe = Client(self)
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError(f"server exited during startup: {self.process.returncode}")
            try:
                expect(probe.request("/api/health", timeout=0.2)["status"], "ok", "health")
                return
            except (urllib.error.URLError, TimeoutError):
                time.sleep(0.025)
        raise AssertionError("server startup timed out")

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=1.5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=1)

    def restart(self):
        self.stop()
        self.start()

    def dump_logs(self):
        self.logs.flush()
        self.logs.seek(0)
        text = self.logs.read().decode("utf-8", errors="replace")
        print("\n--- service logs (all starts) ---\n" + text, file=sys.stderr, flush=True)

    def __enter__(self):
        try:
            self.start()
        except BaseException:
            self.stop()
            self.dump_logs()
            self.logs.close()
            raise
        return self

    def __exit__(self, kind, value, traceback):
        self.stop()
        try:
            if kind is not None:
                self.dump_logs()
            else:
                self.logs.seek(0)
                text = self.logs.read().decode("utf-8", errors="replace")
                for password in PASSWORDS.values():
                    if password in text:
                        self.dump_logs()
                        raise AssertionError("password leaked in service logs")
        finally:
            self.logs.close()


def words(client, book):
    return client.request(f"/api/wordbooks/{book['id']}/words")["items"]


def start(client, book, mode="spelling"):
    return client.request("/api/study/start", {"wordbookId": book["id"], "mode": mode})


def answer_for(state, vocabulary):
    word = vocabulary[state["question"]["wordId"]]
    return word["meanings"][-1] if state["mode"] == "meaning" else word["english"]


def finish(client, state, vocabulary):
    # 限制循环，回归故障应给出断言，而不是耗尽 CTest 的 30 秒超时。
    for _ in range(state["totalCount"] + 1):
        if state["finished"]:
            expect(state["question"], None, "finished question")
            expect(state["currentIndex"], state["totalCount"], "finished index")
            expect(state["passed"], state["accuracy"] >= state["passThreshold"], "round passed")
            return state
        if not state["canAdvance"]:
            state = client.request("/api/study/answer", {"answer": answer_for(state, vocabulary)})
            expect(state["result"]["correct"], True, "correct answer")
        state = client.request("/api/study/next", {})
    raise AssertionError("study did not finish within totalCount next requests")


def assert_user(response, username, user_id=None):
    expect(set(response), {"user"}, "auth response has no token/credentials")
    user = response["user"]
    expect(set(user), {"id", "username", "createdAt"}, "public identity fields only")
    expect(user["username"], username, "current username")
    assert type(user["id"]) is int and user["id"] > 0, user
    assert isinstance(user["createdAt"], str) and user["createdAt"], user
    if user_id is not None:
        expect(user["id"], user_id, "current user id")
    return user


def login(client, username, user_id):
    previous = client.cookie_header()
    result = client.request("/api/auth/login", {
        "username": username, "password": PASSWORDS[username]
    })
    assert_user(result, username, user_id)
    assert client.cookie_header() and client.cookie_header() != previous, "login must rotate Cookie"
    if previous:
        # 框架保留旧 ID 别名的宽限期内，旧 Cookie 也必须立即失去身份与学习权限。
        stale = Client(client.server, previous)
        stale.request("/api/auth/me", status=401)
        stale.request("/api/study", status=401)
    assert_user(client.request("/api/auth/me"), username, user_id)
    client.request("/api/study", status=404)


def assert_statistics(client):
    history = client.request("/api/history")["items"]
    stats = client.request("/api/statistics")
    attempts = sum(row["firstAttemptCount"] for row in history)
    correct = sum(row["correctCount"] for row in history)
    expect(stats["sessionCount"], len(history), "statistics session count")
    expect(stats["completedSessionCount"], sum(row["finished"] for row in history), "completed count")
    expect(stats["firstAttemptCount"], attempts, "retries excluded from denominator")
    expect(stats["correctCount"], correct, "first correct count")
    expect(stats["totalScore"], sum(row["score"] for row in history), "cumulative score")
    expect(stats["totalScore"], 10 * (2 * correct - attempts), "score agrees with first answers")
    near(stats["accuracy"], correct / attempts if attempts else 0, "cumulative first-answer accuracy")
    expect(stats["passed"], bool(attempts and stats["accuracy"] >= stats["passThreshold"]),
           "cumulative passed (not the current round)")
    for row in history:
        near(row["accuracy"], row["correctCount"] / row["totalCount"], "history round accuracy")
        expect(row["passed"], bool(row["finished"] and row["firstAttemptCount"] and
                                   row["accuracy"] >= row["passThreshold"]), "history passed")
        expect(row["finishedAt"] is not None, row["finished"], "finished timestamp")
    return history, stats


def anonymous_tests(anon, isolated):
    books = anon.request("/api/wordbooks")["items"]
    assert books and all(book["isDemo"] for book in books), books
    demo = books[0]
    preview = words(anon, demo)
    count = len(preview)  # 当前二进制的 demo 为 3 词；绝不能套用旧草案的 10 词。
    expect(demo["wordCount"], count, "demo preview count")
    assert count >= 2, "demo must support multiple choice"
    vocabulary = {word["id"]: word for word in preview}
    anon.request("/api/wordbooks/999999/words", status=404)
    anon.request("/api/study", status=404)
    for data in ({"wordbookId": demo["id"], "mode": "bad"},
                 {"wordbookId": str(demo["id"]), "mode": "spelling"},
                 {"wordbookId": 0, "mode": "spelling"}):
        anon.request("/api/study/start", data, status=400)
    anon.request("/api/study/start", {"wordbookId": 999999, "mode": "spelling"}, status=404)
    state = start(anon, demo)
    expect(state["sessionId"], None, "anonymous study is not persistent")
    expect(state["passed"], False, "no attempt cannot pass")
    assert "english" not in state["question"], state
    expect(state["question"]["choices"], [], "spelling hides choices")
    anon.request("/api/study/next", {}, status=409)
    anon.request("/api/study/review", {}, status=409)
    for data in ({"answer": 1}, {"answer": "  "}, {"answer": "bad\x00answer"},
                 {"answer": "a" * 2001}):
        anon.request("/api/study/answer", data, status=400)
    state = anon.request("/api/study/answer", {"answer": WRONG_ANSWER})
    expect((state["score"], state["correctCount"], state["wrongCount"]), (-10, 0, 1), "first wrong")
    expect(state["result"]["firstAttempt"], True, "invalid input did not consume first attempt")
    retry = anon.request("/api/study/answer", {"answer": WRONG_ANSWER})
    expect((retry["result"]["firstAttempt"], retry["result"]["scoreDelta"]), (False, 0), "repeat wrong")
    state = anon.request("/api/study/answer", {"answer": answer_for(state, vocabulary).upper()})
    expect((state["score"], state["canAdvance"]), (-10, True), "retry correct adds no score")
    restored = anon.request("/api/study")
    expect(restored["canAdvance"], True, "Cookie resumes current question")
    assert "result" not in restored, "GET study must not echo old feedback"
    isolated.request("/api/study", status=404)
    other = start(isolated, demo, "meaning")
    expect(anon.request("/api/study")["mode"], "spelling", "different Cookie cannot replace study")
    expect(other["question"]["meanings"], [], "meaning mode hides Chinese answers")
    expect(other["question"]["english"], vocabulary[other["question"]["wordId"]]["english"], "meaning prompt")
    other = finish(isolated, other, vocabulary)
    expect((other["score"], other["correctCount"]), (10 * count, count), "meaning score")
    state = finish(anon, state, vocabulary)
    expect((state["score"], state["correctCount"]), (10 * (count - 2), count - 1), "demo score by actual count")
    state = anon.request("/api/study/review", {})
    expect((state["totalCount"], state["mode"]), (1, "spelling"), "anonymous wrong review")
    finish(anon, state, vocabulary)
    anon.request("/api/study/review", {}, status=400)
    anon.request("/api/study/answer", {"answer": "anything"}, status=409)
    anon.request("/api/study/next", {}, status=409)
    state = start(anon, demo, "choice")
    options = state["question"]["choices"]
    expect(len(options), min(4, count), "choice count follows actual demo size")
    expect(len(set(options)), len(options), "distinct choice options")
    assert answer_for(state, vocabulary) in options and "english" not in state["question"], state
    finish(anon, state, vocabulary)
    start(anon, demo)  # 登录必须清掉这份尚未完成的匿名任务。
    for path in ("/api/auth/me", "/api/history", "/api/statistics", "/api/wrong-words", "/api/checkins"):
        isolated.request(path, status=401)
    isolated.request("/api/checkins", {}, status=401)
    isolated.request("/api/wrong-words/review", {"wordbookId": demo["id"]}, status=401)
    return demo, preview


def account_tests(alice, bob):
    identities = {}
    for client, username in ((alice, "Alice"), (bob, "Bob")):
        response = client.request("/api/auth/register", {
            "username": username, "password": PASSWORDS[username]
        })
        identities[username] = assert_user(response, username)["id"]
        client.request("/api/auth/me", status=401)  # 注册本身不登录。
    assert identities["Alice"] != identities["Bob"], identities
    alice.request("/api/auth/register", {"username": "Alice", "password": PASSWORDS["Alice"]}, status=409)
    wrong = alice.request("/api/auth/login", {"username": "Alice", "password": "wrong-password"}, status=401)
    missing = alice.request("/api/auth/login", {"username": "Nobody", "password": "wrong-password"}, status=401)
    expect(wrong, missing, "login failure must not reveal whether username exists")
    for data in ({"username": "ab", "password": PASSWORDS["Alice"]},
                 {"username": "bad name", "password": PASSWORDS["Alice"]},
                 {"username": "Alice", "password": 123},
                 {"username": "Alice", "password": "short"}):
        alice.request("/api/auth/register", data, status=400)
    login(alice, "Alice", identities["Alice"])
    login(bob, "Bob", identities["Bob"])
    assert_user(alice.request("/api/auth/me"), "Alice", identities["Alice"])
    for client in (alice, bob):
        history, stats = assert_statistics(client)
        expect(history, [], "new account history")
        expect((stats["firstAttemptCount"], stats["accuracy"], stats["passed"]), (0, 0, False), "no-attempt statistics")
        expect(client.request("/api/wrong-words")["items"], [], "new account wrong words")
    return identities


def book_tests(alice, bob, anon, demo, demo_words):
    for data in ({}, {"name": 3}, {"name": " \t "}, {"name": "词" * 51}, {"name": "bad\x00name"}):
        alice.request("/api/wordbooks", data, status=400)
    name = "词" * 48 + "测试"  # 50 Unicode 字符，UTF-8 字节数远大于 50。
    private = alice.request("/api/wordbooks", {"name": " \t" + name + "\r\n"})
    expect((private["name"], private["wordCount"], private["isDemo"]), (name, 0, False), "Unicode/trim name")
    bob_book = bob.request("/api/wordbooks", {"name": " Bob 私人词库 "})
    expect(bob_book["name"], "Bob 私人词库", "Bob name trim")
    expect(words(alice, private), [], "empty private preview is items[], not 404/null")
    alice.request("/api/study/start", {"wordbookId": private["id"], "mode": "spelling"}, status=400)
    valid = {"english": "alpha", "partOfSpeech": "n.", "meanings": ["阿尔法"]}
    anon.request("/api/wordbooks", {"name": "unauthorized"}, status=401)
    for owner, stranger, book in ((alice, bob, private), (bob, alice, bob_book)):
        assert book["id"] in {row["id"] for row in owner.request("/api/wordbooks")["items"]}
        for client in (stranger, anon):
            assert book["id"] not in {row["id"] for row in client.request("/api/wordbooks")["items"]}
            client.request(f"/api/wordbooks/{book['id']}/words", status=403)
            client.request("/api/study/start", {"wordbookId": book["id"], "mode": "spelling"}, status=403)
    alpha = alice.request(f"/api/wordbooks/{private['id']}/words", {
        "english": " alpha ", "partOfSpeech": " n. ", "meanings": [" 阿尔法 ", "阿尔法", "ALPHA", " alpha "]
    })
    expect((alpha["english"], alpha["partOfSpeech"], alpha["meanings"]), ("alpha", "n.", ["阿尔法", "ALPHA"]), "word normalization")
    beta = alice.request(f"/api/wordbooks/{private['id']}/words", dict(valid, english="beta", meanings=["贝塔"]))
    bob_word = bob.request(f"/api/wordbooks/{bob_book['id']}/words", dict(valid, english="bobword", meanings=["鲍勃词"]))
    # 每一种写路由都检查匿名 401、别账号 403，以及公共示例只读 403。
    for client, book, word, status in (
        (anon, private, alpha, 401), (bob, private, alpha, 403),
        (alice, bob_book, bob_word, 403), (alice, demo, demo_words[0], 403)
    ):
        for path, data, method in (
            (f"/api/wordbooks/{book['id']}/words", valid, "POST"),
            (f"/api/wordbooks/{book['id']}/import", {"content": "english\tpos\tmeaning\nx\tn.\t测试", "duplicatePolicy": "merge"}, "POST"),
            (f"/api/words/{word['id']}", {"partOfSpeech": "v."}, "PATCH"),
            (f"/api/words/{word['id']}", None, "DELETE"),
            (f"/api/words/{word['id']}/meanings", {"meaning": "禁止写入"}, "POST")
        ):
            client.request(path, data, method=method, status=status)
    word_path = f"/api/wordbooks/{private['id']}/words"
    for data in ({}, dict(valid, english=2), dict(valid, english="123"),
                 dict(valid, english="a" * 101), dict(valid, partOfSpeech=[]),
                 dict(valid, partOfSpeech="n.\x00"), dict(valid, meanings="错误"),
                 dict(valid, meanings=[]), dict(valid, meanings=[1]),
                 dict(valid, meanings=[" "]), dict(valid, meanings=["释义"] * 51)):
        alice.request(word_path, data, status=400)
    alice.request(word_path, dict(valid, english=" ALPHA "), status=409)
    for data in ({}, {"english": 3}, {"partOfSpeech": []}, {"english": " "}, {"partOfSpeech": "n.\x00"}):
        alice.request(f"/api/words/{beta['id']}", data, method="PATCH", status=400)
    alice.request(f"/api/words/{beta['id']}", {"english": "ALPHA"}, method="PATCH", status=409)
    temporary = alice.request(word_path, dict(valid, english="temporary"))
    patched = alice.request(f"/api/words/{temporary['id']}", {"english": " renamed ", "partOfSpeech": " v. "}, method="PATCH")
    expect((patched["english"], patched["partOfSpeech"]), ("renamed", "v."), "PATCH persists normalized fields")
    expect(next(word for word in words(alice, private) if word["id"] == patched["id"]), patched, "PATCH preview")
    meaning_path = f"/api/words/{alpha['id']}/meanings"
    for data in ({}, {"meaning": []}, {"meaning": " "}):
        alice.request(meaning_path, data, status=400)
    added = alice.request(meaning_path, {"meaning": " 首位 "})
    expect(added["meanings"], ["阿尔法", "ALPHA", "首位"], "append meaning")
    expect(alice.request(meaning_path, {"meaning": "首位"}), added, "duplicate meaning no increase")
    expect(alice.request(meaning_path, {"meaning": " alpha "}), added, "ASCII case-insensitive meaning dedup")
    expect(alice.request(f"/api/words/{temporary['id']}", method="DELETE"), {"success": True}, "DELETE word")
    alice.request(f"/api/words/{temporary['id']}", method="DELETE", status=404)
    assert temporary["id"] not in {word["id"] for word in words(alice, private)}, "deleted word still visible"
    expect(words(bob, bob_book), [bob_word], "forbidden mutations leave Bob data untouched")
    expect(words(anon, demo), demo_words, "public read-only data untouched")
    return private, bob_book


def import_tests(alice, private):
    path = f"/api/wordbooks/{private['id']}/import"
    merge = "﻿english\tpos\tmeaning\r\nALPHA\tadj.\t新增释义\r\nalpha\tn.\t阿尔法\r\nepsilon\t\t艾普西龙\r\nEPSILON\tn.\t第五\r\nepsilon\tn.\t第五\r\n\r\n"
    expect(alice.request(path, {"content": merge, "duplicatePolicy": "merge"}),
           {"imported": 1, "merged": 2, "skipped": 2}, "BOM/CRLF merge summary (actual API fields)")
    skip = "﻿english\tpos\tmeaning\r\nALPHA\tv.\t不应合并\r\nzeta\tn.\t泽塔\r\nZETA\tv.\t不应合并\r\n"
    expect(alice.request(path, {"content": skip, "duplicatePolicy": "skip"}),
           {"imported": 1, "merged": 0, "skipped": 2}, "skip duplicates including same batch")
    preview = words(alice, private)
    by_english = {word["english"]: word for word in preview}
    expect(len(preview), 4, "TSV unique words")
    expect(by_english["alpha"]["partOfSpeech"], "n.", "merge preserves nonempty POS")
    expect(by_english["alpha"]["meanings"], ["阿尔法", "ALPHA", "首位", "新增释义"], "merge meanings")
    expect((by_english["epsilon"]["partOfSpeech"], by_english["epsilon"]["meanings"]),
           ("n.", ["艾普西龙", "第五"]), "in-batch merge fills empty POS")
    expect(by_english["zeta"]["meanings"], ["泽塔"], "skip leaves original meanings")
    # 空行也占物理行号；整批拒绝时，前面的新增/合并均不得落库。
    bad = "﻿\r\nenglish\tpos\tmeaning\r\n\r\nalpha\tn.\t不应保存\r\ntheta\tn.\t不应插入\r\nbroken\tmissing\r\niota\tn.\t\r\nkappa\tn.\t也不应插入\r\n"
    error = alice.request(path, {"content": bad, "duplicatePolicy": "merge"}, status=400)
    expect([row["line"] for row in error["errors"]], [6, 7], "TSV error physical line numbers")
    assert all(row["message"] for row in error["errors"]), error
    expect(words(alice, private), preview, "invalid import rolls back the whole batch")
    for data in ({}, {"content": 1, "duplicatePolicy": "merge"},
                 {"content": "english\tpos\tmeaning", "duplicatePolicy": "replace"}):
        alice.request(path, data, status=400)
    expect(words(alice, private), preview, "invalid import fields do not mutate")
    return {word["id"]: word for word in preview}


def security_tests(alice, private):
    before = words(alice, private)
    path = f"/api/wordbooks/{private['id']}/words"
    data = {"english": "originword", "partOfSpeech": "n.", "meanings": ["同源"]}
    alice.request(path, data, headers={"Origin": "https://foreign.invalid"}, status=403)
    alice.request(path, data, headers={"Sec-Fetch-Site": "cross-site"}, status=403)
    alice.request(path, data, headers={"Origin": alice.server.base, "Sec-Fetch-Site": "cross-site"}, status=403)
    alice.request(path, data, headers={"Content-Type": "text/plain"}, status=400)
    alice.request(path, raw=b"[]", status=400)
    alice.request(path, raw=b"{broken", status=400)
    # 框架的 413 可能不是 JSON，不能套用控制器的错误包格式。
    oversized = json.dumps({"name": "a" * (64 * 1024)}).encode("utf-8")
    assert len(oversized) > 64 * 1024
    alice.request("/api/wordbooks", raw=oversized, status=413, json_response=False)
    expect(words(alice, private), before, "rejected browser/body requests do not mutate")
    created = alice.request(path, data, headers={
        "Origin": alice.server.base, "Sec-Fetch-Site": "same-origin",
        "Content-Type": "application/json; charset=utf-8"
    })
    expect(created["english"], "originword", "same-origin JSON write allowed")
    alice.request(f"/api/words/{created['id']}", method="DELETE")
    expect(words(alice, private), before, "same-origin temporary word cleaned up")
    for origin in ("http://127.0.0.1:5173", "http://localhost:5173"):
        created = alice.request(path, data, headers={
            "Origin": origin, "Sec-Fetch-Site": "same-origin"
        })
        alice.request(f"/api/words/{created['id']}", method="DELETE")
    alice.request(path, data, headers={"Origin": "http://127.0.0.1:5174"}, status=403)
    expect(words(alice, private), before, "fixed Vite proxy origin accepted, arbitrary port rejected")


def checkin_tests(alice, bob, identities):
    second_browser = Client(alice.server)
    login(second_browser, "Alice", identities["Alice"])
    expect(alice.request("/api/checkins"), {"dates": [], "totalDays": 0}, "new checkins")
    expect(bob.request("/api/checkins"), {"dates": [], "totalDays": 0}, "Bob checkins isolated")
    # 两个真实浏览器会话；线程内只发固定 Cookie，不共享可写 CookieJar。
    cookies = [alice.cookie_header(), second_browser.cookie_header()] * 2
    barrier = threading.Barrier(len(cookies))
    shanghai = datetime.timezone(datetime.timedelta(hours=8))
    date_before = datetime.datetime.now(shanghai).date().isoformat()
    def checkin(cookie):
        barrier.wait(timeout=3)
        return Client(alice.server, cookie).request("/api/checkins", {})
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(checkin, cookies))
    date_after = datetime.datetime.now(shanghai).date().isoformat()
    dates = sorted({row["date"] for row in results}, reverse=True)
    for date in dates:
        datetime.date.fromisoformat(date)
        assert date_before <= date <= date_after, f"checkin date is not Shanghai today: {date}"
        expect(sum(not row["alreadyCheckedIn"] for row in results if row["date"] == date),
               1, "same-day multi-browser checkin inserted once")
    expected = {"dates": dates, "totalDays": len(dates)}
    expect(alice.request("/api/checkins"), expected, "checkin persisted without any study")
    expect(second_browser.request("/api/checkins"), expected, "same account browsers share checkins")
    repeated = alice.request("/api/checkins", {})
    if repeated["date"] in dates:  # 允许测试恰好跨过上海午夜。
        expect((repeated["alreadyCheckedIn"], repeated["totalDays"]), (True, len(dates)), "repeat checkin idempotence")
    bob_checkin = bob.request("/api/checkins", {})
    expect((bob_checkin["alreadyCheckedIn"], bob_checkin["totalDays"]), (False, 1), "Bob may check in without learning")
    expect(bob.request("/api/statistics")["firstAttemptCount"], 0, "checkin does not fabricate study")
    return second_browser


def progress_tests(alice, bob, second_browser, private, bob_book, vocabulary):
    for value in ("yes", "1", ""):
        alice.request("/api/wrong-words?mastered=" + value, status=400)
    alice.request("/api/wrong-words/review", {"wordbookId": private["id"]}, status=400)
    state = start(alice, private)
    first_session = state["sessionId"]
    first_word = state["question"]["wordId"]
    expect(state["passed"], False, "unattempted persistent round")
    second_browser.request("/api/study", status=404)
    bob.request("/api/study", status=404)
    cookie = alice.cookie_header()
    barrier = threading.Barrier(4)
    def submit(_):
        barrier.wait(timeout=3)
        return Client(alice.server, cookie).request("/api/study/answer", {"answer": WRONG_ANSWER})
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(submit, range(4)))
    expect(sum(row["result"]["firstAttempt"] for row in results), 1, "concurrent same-Cookie firstAttempt exactly once")
    expect(sorted(row["result"]["scoreDelta"] for row in results), [-10, 0, 0, 0], "concurrent score applied once")
    for row in results:
        expect((row["score"], row["correctCount"], row["wrongCount"]), (-10, 0, 1), "concurrent state")
    state = alice.request("/api/study/answer", {"answer": vocabulary[first_word]["english"]})
    expect((state["result"]["correct"], state["result"]["firstAttempt"], state["result"]["scoreDelta"]),
           (True, False, 0), "retry correct is not first correct")
    wrong = alice.request("/api/wrong-words?mastered=false")["items"]
    expect([(row["word"]["id"], row["wrongCount"], row["mastered"]) for row in wrong],
           [(first_word, 1, False)], "same-round correction does not master word")
    duplicate = alice.request("/api/study/answer", {"answer": vocabulary[first_word]["english"]})
    expect((duplicate["score"], duplicate["result"]["scoreDelta"]), (-10, 0), "duplicate correct no score")
    state = finish(alice, state, vocabulary)
    count = len(vocabulary)
    expect((state["score"], state["correctCount"]), (10 * (count - 2), count - 1), "persistent first round score")
    expect(state["passed"], (count - 1) / count >= state["passThreshold"], "finished threshold")
    history, stats = assert_statistics(alice)
    expect((len(history), stats["firstAttemptCount"]), (1, count), "concurrency/retries persisted once")
    expect(history[0]["id"], first_session, "history session identity")
    expect(alice.request("/api/wrong-words?mastered=true")["items"], [], "nothing mastered yet")
    state = alice.request("/api/study/review", {})
    expect((state["totalCount"], state["question"]["wordId"]), (1, first_word), "last-round review")
    state = finish(alice, state, vocabulary)
    expect(state["passed"], True, "first-correct review passes")
    mastered = alice.request("/api/wrong-words?mastered=true")["items"]
    expect([(row["word"]["id"], row["wrongCount"], row["mastered"]) for row in mastered],
           [(first_word, 1, True)], "later-round first correct masters word")
    expect(alice.request("/api/wrong-words?mastered=false")["items"], [], "mastered filter")
    _, stats = assert_statistics(alice)
    near(stats["accuracy"], count / (count + 1), "review cumulative denominator")
    # 新的未完成轮：保留一个已掌握词，并留下另一个未掌握词供重启后持久复习。
    state = start(alice, private)
    alice.request("/api/study/answer", {"answer": answer_for(state, vocabulary)})
    state = alice.request("/api/study/next", {})
    pending_word = state["question"]["wordId"]
    assert pending_word != first_word, "need another word for persistence isolation"
    state = alice.request("/api/study/answer", {"answer": WRONG_ANSWER})
    alice.request("/api/study/answer", {"answer": answer_for(state, vocabulary)})
    history, stats = assert_statistics(alice)
    pending = next(row for row in history if row["id"] == state["sessionId"])
    expect((pending["finished"], pending["passed"], pending["firstAttemptCount"]), (False, False, 2), "unfinished history persisted")
    assert sum(row["totalCount"] for row in history) > stats["firstAttemptCount"], "partial round must distinguish denominators"
    expect(bob.request("/api/history")["items"], [], "Alice history cannot leak to Bob")
    expect(bob.request("/api/wrong-words")["items"], [], "Alice wrong words cannot leak to Bob")
    # 单词库只有一题：最后一题已答对但还没 next，accuracy=1 仍不能把本轮判过关。
    bob_vocabulary = {word["id"]: word for word in words(bob, bob_book)}
    bob.request("/api/study/start", {"wordbookId": bob_book["id"], "mode": "choice"}, status=400)
    state = start(bob, bob_book)
    history, _ = assert_statistics(bob)
    expect((history[0]["firstAttemptCount"], history[0]["passed"]), (0, False), "no-attempt history passed=false")
    state = bob.request("/api/study/answer", {"answer": answer_for(state, bob_vocabulary)})
    expect((state["accuracy"], state["finished"], state["passed"]), (1, False, False), "high accuracy unfinished round cannot pass")
    history, _ = assert_statistics(bob)
    expect(history[0]["passed"], False, "history requires finished")
    state = finish(bob, state, bob_vocabulary)
    expect(state["passed"], True, "finished perfect round passes")
    start(bob, bob_book)  # logout 必须清除未完成内存任务。
    return pending_word


def snapshot_persistent(client, book):
    history, stats = assert_statistics(client)
    return {
        "books": client.request("/api/wordbooks"), "words": words(client, book),
        "history": history, "statistics": stats,
        "wrong": client.request("/api/wrong-words"),
        "unmastered": client.request("/api/wrong-words?mastered=false"),
        "mastered": client.request("/api/wrong-words?mastered=true"),
        "checkins": client.request("/api/checkins")
    }


def deleted_word_tests(client):
    """删除并重建最大词 ID，不能把旧学习快照或错词集合关联到新词。"""
    for same_english in (True, False):
        book = client.request("/api/wordbooks", {"name": "删除快照回归"})
        original = client.request(f"/api/wordbooks/{book['id']}/words", {
            "english": "snapshotword", "partOfSpeech": "n.", "meanings": ["旧释义"]
        })
        state = start(client, book)
        if not same_english:
            client.request("/api/study/answer", {"answer": WRONG_ANSWER})
        client.request(f"/api/words/{original['id']}", {}, method="DELETE")
        replacement = client.request(f"/api/wordbooks/{book['id']}/words", {
            "english": "snapshotword" if same_english else "replacementword",
            "partOfSpeech": "v.", "meanings": ["全新释义"]
        })
        assert replacement["id"] > original["id"], "deleted word IDs must never be reused"
        if same_english:
            state = client.request("/api/study/answer", {"answer": WRONG_ANSWER})
            expect(state["score"], -10, "stale snapshot scoring still valid")
        client.request("/api/study/answer", {"answer": original["english"]})
        state = client.request("/api/study/next", {})
        expect(state["finished"], True, "stale snapshot can finish without corrupting current words")
        assert replacement["id"] not in {
            entry["word"]["id"] for entry in client.request("/api/wrong-words")["items"]
        }, "replacement must not inherit the deleted word's mistakes"
        client.request("/api/study/review", {}, status=400)
        client.request("/api/wrong-words/review", {"wordbookId": book["id"]}, status=400)


def threshold_tests(binary, directory):
    # 无效 CLI 值必须立即失败，不能落入 app.run()；不为这些进程创建账号。
    for value in ("-0.01", "1.01", "nan", "inf", "-inf", "0.8junk", "", "not-a-number"):
        try:
            result = subprocess.run(
                [binary, "--database", str(directory / "invalid.sqlite"), "--pass-threshold", value],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=2, cwd=str(directory)
            )
        except subprocess.TimeoutExpired as error:
            raise AssertionError(f"--pass-threshold {value!r} did not reject/exit promptly; output={error.output!r}") from error
        expect(result.returncode, 1, f"invalid --pass-threshold {value!r}: {result.stdout!r}")
        assert result.stdout, f"invalid threshold {value!r} needs a startup diagnostic"
    result = subprocess.run([binary, "--pass-threshold"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=2, cwd=str(directory))
    expect(result.returncode, 1, "missing threshold value")
    for threshold in (0, 1):
        with Server(binary, directory / f"threshold-{threshold}.sqlite", threshold) as server:
            client = Client(server)
            demo = client.request("/api/wordbooks")["items"][0]
            vocabulary = {word["id"]: word for word in words(client, demo)}
            state = start(client, demo)
            expect(state["passThreshold"], threshold, "inclusive CLI boundary")
            expect(state["passed"], False, "even threshold=0 needs a finished round")
            if threshold == 0:
                state = client.request("/api/study/answer", {"answer": WRONG_ANSWER})
            state = finish(client, state, vocabulary)
            expect(state["passed"], True, f"boundary threshold {threshold} completed round")
            if threshold == 1:
                state = start(client, demo)
                state = client.request("/api/study/answer", {"answer": WRONG_ANSWER})
                state = finish(client, state, vocabulary)
                expect(state["passed"], False, "threshold=1 rejects even one first wrong")


def run(binary):
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="englishword-api-") as temporary:
        directory = pathlib.Path(temporary)
        with Server(binary, directory / "integration.sqlite") as server:
            alice, anon, bob = Client(server), Client(server), Client(server)
            demo, demo_words = anonymous_tests(alice, anon)
            print("Anonymous scoring, modes and Cookie isolation passed.", flush=True)
            identities = account_tests(alice, bob)
            private, bob_book = book_tests(alice, bob, anon, demo, demo_words)
            vocabulary = import_tests(alice, private)
            security_tests(alice, private)
            print("Accounts, private CRUD, TSV atomicity and request security passed.", flush=True)
            second_browser = checkin_tests(alice, bob, identities)
            pending_word = progress_tests(alice, bob, second_browser, private, bob_book, vocabulary)
            alice_before = snapshot_persistent(alice, private)
            bob_before = snapshot_persistent(bob, bob_book)
            previous_cookie = bob.cookie_header()
            bob.request("/api/auth/logout", {"checkin": "true"}, status=400)
            assert_user(bob.request("/api/auth/me"), "Bob", identities["Bob"])
            logged_out = bob.request("/api/auth/logout", {"checkin": True})
            expect(logged_out["success"], True, "logout success")
            checkin = logged_out["checkin"]
            if checkin["date"] in bob_before["checkins"]["dates"]:
                expect((checkin["alreadyCheckedIn"], checkin["totalDays"]),
                       (True, bob_before["checkins"]["totalDays"]), "logout checkin is idempotent")
            else:
                bob_before["checkins"]["dates"].insert(0, checkin["date"])
                bob_before["checkins"]["totalDays"] += 1
            assert bob.cookie_header() != previous_cookie, "logout must rotate Cookie"
            bob.request("/api/auth/me", status=401)
            bob.request("/api/study", status=404)
            bob.request("/api/checkins", status=401)
            expect(alice.request("/api/auth/me")["user"]["id"], identities["Alice"], "Bob logout cannot affect Alice")
            print("Concurrent answers, persistent progress, checkins and logout passed.", flush=True)
            # 同一个 SQLite 文件，真实停服/重启，而非重新创建客户端模拟重启。
            server.restart()
            for client in (alice, bob, second_browser):
                client.request("/api/auth/me", status=401)
                client.request("/api/study", status=404)
            login(alice, "Alice", identities["Alice"])
            login(bob, "Bob", identities["Bob"])
            expect(snapshot_persistent(alice, private), alice_before, "Alice data survives restart exactly")
            expect(snapshot_persistent(bob, bob_book), bob_before, "Bob data survives restart exactly")
            for client, forbidden in ((alice, bob_book), (bob, private)):
                client.request(f"/api/wordbooks/{forbidden['id']}/words", status=403)
                client.request("/api/wrong-words/review", {"wordbookId": forbidden["id"]}, status=403)
            expect(bob.request(f"/api/history?userId={identities['Alice']}")["items"],
                   bob_before["history"], "query userId cannot override Cookie identity")
            bob.request("/api/wrong-words/review", {"wordbookId": bob_book["id"]}, status=400)
            state = alice.request("/api/wrong-words/review", {"wordbookId": private["id"]})
            expect((state["mode"], state["totalCount"], state["question"]["wordId"]),
                   ("spelling", 1, pending_word), "persistent wrong review works without old memory task")
            finish(alice, state, vocabulary)
            expect(alice.request("/api/wrong-words?mastered=false")["items"], [], "persistent review masters outstanding word")
            expect(len(alice.request("/api/wrong-words?mastered=true")["items"]), 2, "both mastered records retained")
            assert_statistics(alice)
            expect(snapshot_persistent(bob, bob_book), bob_before, "Alice review cannot change Bob persistence")
            print("Same-database restart, lost memory tasks and durable review passed.", flush=True)
            deleted_word_tests(bob)
            print("Deleted-word identities and stale snapshot review passed.", flush=True)
        threshold_tests(binary, directory)
    print(f"HTTP API integration passed in {time.monotonic() - started:.2f}s.", flush=True)


if __name__ == "__main__":
    run(str(pathlib.Path(sys.argv[1]).resolve()))
