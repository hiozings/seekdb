# 开发验证记录

本文件记录实施过程中的决策与可复现的验证结果，作为后续材料整理的原始依据。
只记录实际执行过的命令与真实输出；计划中的内容明确标注「计划」。

## 2026-11-09

### 背景确认

- 读取 `2026-competition-round2` 分支内容：该分支删除了 seekdb 全部源码与构建系统，
  仅保留 37 个脚手架文件，以及两个 0 字节占位文件 `Dockerfile`、`conversation.json`。
  结论：本题是从零实现，不能复用 seekdb 代码。
- 赛题要求的评测入口是 `agent/conversation.json`，仓库根目录的同名空文件只是占位。

### 决策

| 决策 | 理由 |
| --- | --- |
| 语言选 C++20 + CMake | 本机 GCC 16.2.1 / Clang 23 可用；无第三方依赖，容器构建可锁定；便于写 SIMD 距离核 |
| 优先做协议层 | 题包协议未发布，输出格式错一个字符就会导致功能用例全灭 |
| 线格式全部走 `FormatSpec` | 题包发布后只改配置或一处映射表，不动引擎 |
| 本地项目 vecstor-new 不复用代码 | 其存储/事务基于 DuckDB、ANN 基于 Faiss，均为赛题点名禁止；运行形态（三进程 gRPC）也与赛题不符 |
| 只迁移设计经验 | 过滤优先/检索优先的取舍、"凑够 K 就停"的缺陷、索引版本与退化、校准/留出分离 |

### 实施与验证

1. 创建隔离工作树：`git worktree add ../seekdb-round2 -b feature/minivectordb 2026-competition-round2`，
   避免影响原工作区 `feature/fts-performance-20261101` 上的未提交改动。
2. 实现 M0 骨架：协议层（语句切分 / FormatSpec / 输出写入）、SQL 词法+递归下降解析、
   内存执行引擎（DDL/DML/SELECT/L2_DISTANCE/事务快照）、多阶段 Dockerfile。
3. 构建验证：

   ```
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # 成功
   cmake --build build -j32                          # 成功，产出 build/minivectordb
   ```

4. 本地一致性用例（`tests/conformance/driver.py`）：

   ```
   [ok]      m0_basic.test
   [ok]      m0_errors.test
   [ok]      m0_txn.test
   [ok]      m0_vector.test

   4/4 passed
   ```

5. 容器验证（`docker build -t minivectordb:dev .` 成功，运行输出与本地一致）：

   ```
   docker run --rm -i --cpus=2 --memory=2g -v mvdb-dev-data:/data minivectordb:dev < smoke.sql
   id	content	distance
   1	事务与恢复	0.141421
   2	向量索引	1.272792
   id
   1
   2
   3
   ```

6. 协议边界验证：

   ```
   printf 'CREATE TABLE t (id BIGINT PRIMARY KEY);\nINSERT INTO t VALUES (1);;\nquit\nINSERT ...' | ./build/minivectordb
   # 退出码 0，quit 之后不再执行；连续分号与 EOF 前缺分号均正确处理
   ```

   发现并修复：裸 `quit` 行若无分号会留在切分缓冲区直到 EOF，导致会话无法立即结束。
   修复方式是在缓冲区为空时对整行做退出命令判定。

### 观察（待进一步验证）

- 本机项目 vecstor-new 的精确检索使用 NumPy **float64**，而赛题要求
  **float32 存储后计算**。两者真值在 128 维、较大值域下可能产生 1e-5 量级差异，
  在文本精确比对时会表现为失败。**计划**：在 M2 做一次 float32 vs float64 的
  真值差异实测，把差异量级记录下来，作为参考真值的口径依据。
- 赛题要求「过滤后的 TopK 必须在满足 WHERE 的集合内计算」。
  当前 M0 是逐行过滤后再排序，天然满足；**风险**在 M4 引入索引后被无意破坏，
  **计划**：M4 增加"先全表取 K 再过滤"的错误实现对照，确认用例能捕获该缺陷。

### 计划（未执行）

- M1：页存储、B+tree 主键索引、catalog 持久化，重启后数据一致。
- M3：undo + WAL + checkpoint，kill -9 崩溃恢复验证。
- M4：HNSW 与校准脚本，留出集 Recall@10 验证。
