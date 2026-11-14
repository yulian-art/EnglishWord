# EnglishWord · 个性化背单词

三人课程项目的可运行骨架。**前端固定 React，后端及背诵业务使用 C++17。**

技术栈：React + TypeScript + Vite + Ant Design + React Router + TanStack Query；C++17 + Drogon + SQLite + CMake。

![学习概览预览](docs/assets/preview.png)

## 当前进度

后端已实现注册、登录、退出和用户数据隔离，私人词库及单词增删改、多释义维护、UTF-8 TSV 整批事务导入，以及历史成绩、长期错题本、学习统计和按上海日期打卡。示例词库目前含三个单词；三种学习模式均由 C++ 判题，每题首次答对 +10、首次答错 −10，即时重试不重复计分。

登录用户的有效作答、成绩、错题和打卡保存在 SQLite，重启后重新登录仍可查询；当前题目位置只保存在临时会话，闲置一小时或后端重启后不会恢复。匿名用户可学习公共示例，但不保存个人历史。过关判定已实现，默认正确率 80%，可用 `--pass-threshold 0.8` 配置；最终标准仍待老师确认。

测试包含纯业务、数据库及升级、密码、进度和真实 HTTP 集成五项，覆盖两账号隔离、事务失败回滚、并发首答和打卡幂等、重启持久化。前端页面接入和未解决的前端合并冲突尚需单独处理，本次后端测试不代表整站或新电脑演示已通过。C++ 与 React 混合语言是否符合课程要求，应按老师口径确认。

## 目录

```text
backend/
  src/core/           # 纯 C++ 业务：单词、判题、计分、错题
  src/controllers/    # Drogon 路由与请求校验
  src/repositories/   # 通过 Drogon 数据库客户端访问 SQLite
  db/migrations/      # 数据结构及版本迁移
  data/demo.tsv       # UTF-8 词库导入格式示例
frontend/
  src/pages/          # 学习概览、词库、背诵、复习、待开发页面
  src/lib/api.ts      # API 类型与请求封装
docs/                 # 分工计划、接口和架构
tests/                # C++ 单元测试与 Python HTTP 集成测试
```

## 本地开发

当前电脑的项目地址与可直接复制的启动命令见 [本地启动速查](docs/LOCAL.md)。

需要 C++17 编译器、CMake 3.24+、Node.js 22.12+ 和 Python 3（用于 HTTP 集成测试）。Windows 建议安装 Visual Studio 2022 Build Tools 的“使用 C++ 的桌面开发”，在开发者终端执行构建命令。前后端均不依赖 Qt。

### 1. 安装后端依赖

推荐使用 vcpkg，仓库已固定依赖基线，仅开启 Drogon SQLite 功能。首次构建会编译依赖，需要联网。

Windows PowerShell，在仓库根目录执行：

```powershell
git clone https://github.com/microsoft/vcpkg.git .tools/vcpkg
git -C .tools/vcpkg checkout ec48c11eae9448c8c6e7739d93e06fa2433aa95f
.\.tools\vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake --preset dev "-DCMAKE_TOOLCHAIN_FILE=$PWD/.tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset dev --config Debug --parallel 2
ctest --preset dev -C Debug
```

Linux / WSL，在仓库根目录执行（先安装编译工具、Git、curl、zip、unzip、tar、pkg-config）：

```bash
git clone https://github.com/microsoft/vcpkg.git .tools/vcpkg
git -C .tools/vcpkg checkout ec48c11eae9448c8c6e7739d93e06fa2433aa95f
./.tools/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake --preset dev -DCMAKE_TOOLCHAIN_FILE="$PWD/.tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset dev --parallel 2
ctest --preset dev
```

如果系统已安装启用 SQLite 的 Drogon 开发包，直接运行 `cmake --preset dev` 即可；编译和测试命令不变。

### 2. 启动 C++ 后端

在仓库根目录执行：

```bash
# Linux / WSL，或使用单配置生成器的 Windows
./build/dev/backend/englishword_server --port 8080
```

Windows 使用 Visual Studio 多配置生成器时，可执行文件为：

```powershell
.\build\dev\backend\Debug\englishword_server.exe --port 8080
```

默认数据库：`build/runtime/englishword.sqlite`。启动时自动建表、迁移旧版数据和导入示例，不会创建演示账号；无需删除旧数据库。可用 `--database 路径` 改变位置、`--pass-threshold 0.8` 设置过关阈值，`--help` 查看参数。

### 3. 启动 React 前端

另开终端：

```bash
cd frontend
npm ci
npm run dev
```

浏览器访问 [http://127.0.0.1:5173](http://127.0.0.1:5173)。Vite 将 `/api` 代理到 `127.0.0.1:8080`，浏览器无需配置跨域。请始终使用同一个浏览器地址，避免混用 localhost 与 127.0.0.1 导致 Cookie 不同。

## 检查与演示

```bash
# 无外部依赖也能验证纯 C++ 业务
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only

# 前端类型检查及生产构建
cd frontend
npm run build
```

前端构建完成后，在仓库根目录启动后端并指定静态资源目录：

```bash
./build/dev/backend/englishword_server --web-root frontend/dist
```

此时访问 [http://127.0.0.1:8080](http://127.0.0.1:8080)，由同一个 C++ 服务提供前端和 API。前端使用 HashRouter，刷新页面不需要额外配置 SPA 回退。

## 三人接力

每人从 `main` 创建 `codex/模块名` 分支，每天提交可运行的小改动，通过 PR 合并。

- [一周分工与验收](docs/PLAN.md)
- [API 与字段约定](docs/API.md)
- [数据库字段中文说明与表关系](docs/DATABASE.md)
- [业务例子与答辩口径](docs/BUSINESS.md)
- [本地地址与启动命令](docs/LOCAL.md)
- [架构、业务规则与扩展点](docs/ARCHITECTURE.md)

不提交数据库文件、账号密码、Token 或本机依赖目录。作业原始 PDF 不在公开仓库中。

## 官方资料

- [Drogon：路由、会话、数据库及代码生成工具](https://github.com/drogonframework/drogon)
- [React](https://react.dev/learn)
- [Ant Design 组件](https://ant.design/components/overview-cn/)
- [Vite](https://vite.dev/guide/)
- [TanStack Query](https://tanstack.com/query/latest/docs/framework/react/overview)

使用 Drogon 提供的基础设施与 Ant Design 提供的组件，团队主要编写课程业务。后续需要 CRUD 模型时，可安装 Drogon 的 `ctl` 功能，使用 `drogon_ctl` 生成控制器或数据库模型；当前骨架不依赖生成工具才能运行。
