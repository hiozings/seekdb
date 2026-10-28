# 本地 AI 协议测试

本目录提供不依赖真实模型的确定性 Mock，以及执行真实 seekdb SQL 的集成测试。它们是题面的本地替代测试，不是组委会正式评测。

## 运行 SQL 集成测试

先构建并启动**专用测试实例**，例如监听 `127.0.0.1:12882`。测试使用 mysql CLI、Python 3 标准库，不需要安装 Python 第三方包。

```bash
AI_TEST_PORT=12882 python3 tools/deploy/mysql_test/test_suite/ai_function/mock/test_ai_sql.py
```

测试自行在 `0.0.0.0:8000` 启停 Mock。该端口应空闲，且测试进程和数据库应在同一网络命名空间；在容器中部署时，将两者放在同一容器即可。测试会创建/删除 `ai_enhancement_test` 数据库及 `local_*` 模型和 Endpoint，勿对业务实例执行。测试账户为该专用实例的无密码 root，与仓库已有本地部署方式一致。

测试覆盖六个子题、600 行混合维度、NULL/过滤/LIMIT/CASE、乱序响应、Rerank 跨批同分稳定排序、Endpoint ALTER 后预处理语句、Dashscope 批量兼容性、错误返回和第二批失败后的恢复。结束时输出 Mock 请求数量和实际最大 Embedding 输入批量。

## 单独运行 Mock 与 mysqltest

```bash
python3 tools/deploy/mysql_test/test_suite/ai_function/mock/mock_ai_service.py --port 8000
```

另一个终端运行：

```bash
deps/3rd/u01/obclient/bin/mysqltest \
  --host=127.0.0.1 --port=12882 --user=root --database=test \
  --test-file=tools/deploy/mysql_test/test_suite/ai_function/local/ai_function_enhancement_custom.test \
  --result-file=tools/deploy/mysql_test/test_suite/ai_function/local/ai_function_enhancement_custom.result
```

也可在已准备好 OBD 账户和连接变量的环境中使用 `obd.sh mysqltest --suite ai_function`。原有部分 DDL/权限用例依赖 OBD 的 `admin` 账户和初始化变量，不能把裸实例上的缺少账户当作产品失败。

Mock 支持 OpenAI Completion/Embedding、Ollama 原生生成/Embedding、Rerank 和 Dashscope Embedding；向量由输入的 SHA-256 确定，维度由请求决定，默认 4 维仅是测试夹具约定。OpenAI/Dashscope 刻意逆序返回带 index 的向量，Rerank 刻意返回未排序结果。故障由 URL 路径选择，数据库实现中没有模型名或测试输入特判。`GET /requests` 返回当前 Mock 捕获的请求。

## C++ 单测

```bash
./build.sh release --make -j2
./build.sh release -DSEEKDB_BUILD_COMPETITION_TESTS=ON
cmake --build build_release --target competition_unit_tests --parallel 2
ctest --test-dir build_release -L competition --output-on-failure
```

当前比赛基线采用 `build.sh`/CMake；生产构建不会自动构建测试目标。目标及依赖说明见 [unittest/README.competition.md](../../../../../../unittest/README.competition.md)。上述命令不是 Sanity/ASan 插桩验证。

## 本地固定的语义

- AI_EMBED 保留 JSON 数组文本返回类型；未传 dimension 时使用返回维度，不硬编码模型维度；NULL content 返回 SQL NULL。
- Prompt 保留 `{template,args}` JSON 结构。字符串直接渲染，JSON Array/Object 序列化，合法嵌套 Prompt 递归渲染。闭合占位符必须是有效数字下标；未闭合左花括号作为文本保留。参数内容不重复解释。
- AI_COMPLETE 的省略/SQL NULL config 等价；OpenAI config 不允许覆盖 model/messages/stream。响应必须是非空字符串。
- Rerank 要求返回全部输入的唯一 index，按分数降序、原始 index 升序排序；对象输入保留原始 ID 与字段。
- 一般 Embedding 请求最多 64 条，Dashscope 原生请求最多 10 条，另按约 1 MiB 原始文本拆分；单条长输入不拆成多行。Prompt 渲染和 HTTP 响应分别限制为 16 MiB；嵌套 Prompt 最深 64 层。

上述约定描述自建 Mock 与回归覆盖。比赛分支已包含官方测试，验证官方契约应使用下述官方运行器，不能用这些自建结果代替。

## 历史验证记录（2026-10-26）

基于提交 `911342cd7`，在当时的本地 Linux 容器专用实例执行。以下记录不能证明当前比赛基线构建或官方测试通过：

- 普通 seekdb 构建成功。
- 4 个 C++ 测试目标、13 个测试通过。
- 14 组 SQL/Mock 集成测试通过，捕获 98 个请求，最大 Embedding 请求携带 64 条输入。
- 当时的自建 `ai_function_enhancement`、自建增强版 `ai_prompt`（现保存在 `ai_function/local/`）、`ai_model_lower_case_name_mysql1`、普通表达式 `func_length` 的 mysqltest 结果比对通过。
- 连续 30 次网络错误后的恢复测试通过，测试实例文件描述符数量前后均为 18；这不是内存泄漏检测的替代。
- 独立代码审查指出的模板扫描性能、Dashscope 批量上限和响应校验问题已修复，并增加回归覆盖。

当时的验证限制：依赖缺少 `sanity_pass` 插件，`--sanity` 在 Bazel 分析阶段失败，没有运行插桩测试。原有 DDL/权限 mysqltest 依赖 OBD 的 admin 初始化；自动审批拒绝了创建全权限 admin 的准备动作，因此未运行这部分原有用例。新增本地测试已验证 Endpoint 创建/查询调用/修改/删除及预处理语句更新，但不替代原有权限矩阵测试。未调用真实模型，也没有正式评测成绩。

## 当前官方用例入口

```bash
python3 unittest/competition/run_official.py --verify-only
python3 unittest/competition/run_official.py --port 12883 ai_prompt ai_function_enhancement
```

该运行器使用官方 `.test` 与 `.result`，保留测试正文和预期结果字节不变；正式执行时只在临时目录复制预期并保存日志。省略用例名可运行九组官方用例，File SQL 需要另外提供正确的 `--socket`。详见 [官方运行器说明](../../../../../../unittest/competition/README.md)。

本目录的 `mock_ai_service.py` 和 `test_ai_sql.py` 仍只服务于自建回归，默认 4 维；不能直接用它们的结果宣称官方 enhancement 通过。官方运行器使用 `unittest/competition/official_ai_fixture.py` 作为本地协议夹具，默认 768 维、显式维度遵循请求，原始请求写入证据目录。运行前端口 8000 必须空闲，数据库和 Mock 共享网络命名空间。这是本地官方测试正文验证入口，不代表真实模型评测成绩。
