# 官方比赛用例的本地运行

此运行器适用于已经启动的**专用、可丢弃测试实例**。它不启动或构建数据库，不连接练习实例。默认端口 12883，无密码 root，数据库 `test`；File SQL 还需要同一实例的本地 socket。数据库与运行器必须共享网络命名空间及 `/tmp` 文件系统。

```bash
python3 unittest/competition/run_official.py --verify-only
python3 unittest/competition/test_runner.py
python3 unittest/competition/run_official.py --port 12883 --socket /path/to/run/sql.sock
# 可选择单独用例，例如：
python3 unittest/competition/run_official.py --port 12883 ai_column_t1_dml ai_prompt
```

默认依次运行五组 AI COLUMN、AI Prompt、AI function enhancement、AI Endpoint DDL、File SQL basic。命令的退出码非零即存在失败；不会遇错后生成新预期，也不会屏蔽产品 SQL 错误。`mysqltest.log`、`.reject`、命令参数、预期副本、官方 SHA-256 清单和 summary.json 留在打印出的 `/tmp/seekdb-official-*` 目录。可以使用 `--mysqltest` 和 `--mysql` 指定客户端。`--timeout` 默认每个用例 900 秒。

运行前后检查提交 `5bce9e0` 涉及的 30 个官方文件逐字节一致，同时检查 File SQL 的 `t/` 只有官方基础用例、`data/` 精确包含四个官方文件。官方 `.test` 文件直接 source，官方 `.result` 复制到临时目录后用于比对，绝不使用 `--record`。

唯一 include 覆盖是 File SQL 的两个 SSH/admin 部署脚本。临时工作目录保留相同相对 include 路径，其内容明确指向 Python 运行器执行部署；测试正文、SQL、错误断言、输出比对均原样保留。运行器从官方 data 复制三份文本并解码 Parquet，在测试前记录精确文件清单，通过指定 socket 暂时设置 `secure_file_priv`，最后恢复原值并清除本次数据目录。此过程要求 socket 与 TCP 端口指向同一专用实例。

AI COLUMN 使用逐字节复制的官方 mock，端口 19121–19124 必须空闲。测试失败时运行器也会保存 mock 状态并停止本次 mock。AI function enhancement 没有配套官方 mock，使用本目录的 `official_ai_fixture.py`：8000 端口必须空闲；默认返回 768 维以匹配官方预期，显式维度来自请求；Completion 回显请求文本，Rerank 依查询词重叠打分，Embedding 与 Rerank 反序返回以检查 index 映射。它不读取 SQL 或结果文件，不修改产品实现，记录收到的请求到 `mock-requests.json`。这个协议夹具只验证本地协议契约，不声称代表真实外部模型。

`test_runner.py` 的离线检查验证官方文件约束、预期副本与 `/bin/false` 的失败传播，以及 mock 的维度和排序行为；通过这些检查不等于官方数据库测试通过。

## 进程中断后的持久任务恢复

`test_ai_recovery.py` 是额外的恢复回归，独立于上述九组官方 mysqltest。先协调容器内存，再在隔离测试容器中执行：

```bash
python3 /work/unittest/competition/test_ai_recovery.py \
  --binary /work/build_release/src/observer/seekdb --memory-limit 1G
```

脚本只连接自己启动的 12885 专用实例，19129 为自己持有的 Mock；两端口必须空闲。它保留新建的数据目录，记录任务进入 RUNNING 与模型请求开始，使用明确记录的 SQL 故障注入将该任务 `lease_until` 设为零，再 SIGKILL 自己的数据库子进程。Mock 首次响应在数据库死亡后释放，因此无法写回。随后以同一二进制、同一目录重启，断言严格读取成功、结果正确、任务排空，且总计两次模型请求（一笔预期重复调用）；已回滚的业务行不得产生请求。

官方 Mock 文件不变。运行时只改为本次证据目录并控制 `slow-old` 的延迟，响应生成保留官方逻辑。该注入加速租约到期，因此测试证明中断后的过期任务恢复，不衡量真实租约等待时长。脚本失败也会停止并回收自己持有的数据库和 Mock 子进程；日志、SQL 记录、任务快照与结果保留于打印的 `/tmp/seekdb-ai-recovery-*`。准备或语法检查通过不等于实际恢复测试通过。


## AI Endpoint DDL 权限夹具

默认运行集包含官方 `ai_model_endpoint_ddl.test`，覆盖 `MAX_CONCURRENCY` 与权限矩阵。运行器只在专用可丢弃实例上准备 `admin@127.0.0.1`；任意 host 已存在 `admin` 或 `test_ai_user` 时直接拒绝，不覆盖现有账号。只授予 CREATE USER、test.* 的 SELECT、CREATE/ALTER/DROP AI MODEL；后四项带 GRANT OPTION，以便官方测试逐步授权。另授予 mysql.user 的 SELECT（不带转授权），供官方 SHOW GRANTS 查看其他测试用户的授权；没有 ALL 或 SUPER。MySQL 的 GRANT OPTION 按权限范围工作，不能把全局授权能力理解为独立于其他已持有全局权限。

密码每次随机生成，仅通过 mysql 标准输入和权限 0600 的临时 driver 传递；driver 在 finally 删除，日志中若出现该随机密码则脱敏。官方 test_ai_user 创建/授权/断言仍由原始测试执行；结束或失败时回收本次拥有的两个账号，部分 setup 失败也回收已创建 admin。失败清理会记录并使运行失败；不会为通过用例拓宽权限。此夹具不修改任何全局设置。本地执行结果见 `docs/validation/2026-10-28-evaluation-fixes/official-summary.json`；离线检查本身不代表数据库权限矩阵通过。
