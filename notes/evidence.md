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

## 2026-11-09（M1：持久化存储）

### 决策

| 决策 | 理由 |
| --- | --- |
| 页式存储 + 有界 buffer pool（256 页 = 2 MiB） | 题包明确要求支持超过内存预算的数据集，不能全量载入 |
| `pread/pwrite` 而非 `mmap` | 平台的 M 指标含页缓存，`mmap` 会把整个文件计入内存峰值 |
| 页用 `shared_ptr` 管理 | 缓存换出不能使调用方手里的页指针失效 |
| 行编码按模式紧凑存放（无逐值类型标签） | 减少 I/O 字节，直接影响性能项的 B 指标 |
| 主键索引先用"打开时全表扫描重建"的临时方案 | 先打通持久化链路；M1b 再换磁盘 B+tree，避免一次性引入两类风险 |
| 正常关闭时回滚未提交事务 | 赛题要求；实现后由 `txn_durability.restart` 用例固化 |

### 实施与验证

1. 新增 `src/storage/`：槽页、页缓存、堆文件、catalog、行编解码。
2. 执行引擎改写为存储驱动（`src/exec/engine.*`），事务回滚从"整库快照"改为内存 undo 链。
3. 一致性用例扩展为 6 组，新增跨重启用例（`tests/conformance/restart/`）：

   ```
   [ok] m0_basic.test      [ok] m0_errors.test   [ok] m0_txn.test
   [ok] m0_vector.test     [ok] persistence.restart   [ok] txn_durability.restart
   6/6 passed
   ```

4. 大数据集验证（10.4 MiB 数据、2 MiB 页缓存）：

   ```
   rows=20000 dim=128 batch=100
   insert wall time = 2.08s, peak RSS = 8.4 MiB
   verify wall time = 0.08s, peak RSS = 11.3 MiB
   [ok] first id / last id / point lookup / full scan row count / full scan is ordered
   ```

### 发现并修复的两个真实缺陷

1. **RID 哨兵值错误**：最初用 `rid == 0` 表示分配失败，但 `(page 0, slot 0)` 的 RID 本身就是 0，
   导致第一条记录必然被判失败（现象：`ERROR 1105: cannot allocate space for row`）。
   改为独立的 `kInvalidRid` 哨兵。
2. **未提交事务在进程退出时会落盘**：进程读到 EOF 直接退出，析构里的 `sync()` 把脏页写了下去，
   重启后能看到未提交的插入（由 `txn_durability.restart` 第三段捕获）。
   修复：新增 `Engine::shutdown()`，先回滚未提交事务再刷盘，`main` 在退出前调用。

   第二个缺陷值得记录：它正是"正常关闭须回滚未提交事务"这条赛题要求对应的实现点，
   而用例里加入"提交 → 重启 → 未提交 → 重启"的对照才让它暴露。

### 计划（未执行）

- M1b：磁盘 B+tree 主键索引，替换启动时的全表扫描重建。
- M3：WAL + checkpoint，覆盖强制终止（kill -9）场景。
- M4：HNSW 与校准脚本。

## 2026-11-09（M1b：磁盘 B+tree 主键索引）

### 决策

| 决策 | 理由 |
| --- | --- |
| 自研磁盘 B+tree，页 0 作元数据页 | 主键索引内存占用不能随行数线性增长；启动也不能靠全表扫描 |
| 叶子存 `(key, rid)`，内部节点 `count` 个分隔键 + `count+1` 个子页 | 标准 B+tree 布局，路由规则简单 |
| 删除只摘键、不合并节点 | 路由只需"最右且分隔键 ≤ key"的子页，删空叶子不影响正确性；先保证正确再谈空间回收 |
| 用 `built` 标志位而非"行数 > 0"判断是否需重建 | 索引构建被打断时标志未置位，下次启动重建；`insert` 是 upsert，重建幂等 |
| 页缓存在换出时跳过被调用方持有的页（`use_count > 1`） | 否则 B+tree 分裂过程中分配新页可能把正在修改的父页换出，之后重新 fetch 会拿到陈旧副本 |

### 实施与验证

1. 新增 `src/storage/btree.{h,cpp}`，`tests/storage/btree_test.cpp`（随机插入/删除/
   查找对照 `std::map`，16 页缓存，含重开验证）。
2. 引擎把 `pk_map` 换成 `BTree`，启动时按 `built` 标志决定是否重建。

   ```
   btree_test: all checks passed
   [ok] m0_basic / m0_errors / m0_txn / m0_vector
   [ok] index_restart.restart   # 重启后仍能查重、删除后主键可复用
   [ok] persistence.restart  [ok] txn_durability.restart
   7/7 passed
   ```

3. 规模验证（10 万行 × 128 维，51.9 MiB 数据、2 MiB 页缓存）：

   ```
   insert wall time = 1.84s,  peak RSS = 9.3 MiB
   verify wall time = 0.12s,  peak RSS = 29.3 MiB
   ```

   对比 M1 阶段同一负载：insert 53.32s、verify 0.19s —— 启动时间不再随行数增长。

### 发现并修复的性能缺陷（O(n²) 插入）

`HeapFile::insert` 在 hint 页放不下时会**从第 0 页回扫整个文件**寻找空位；
而这个回扫在"需要分配新页"时必然触发一次。于是每写满一页就扫描一遍全文件，
插入退化为 O(n²)。实测同一负载的耗时：

| 行数 | 修复前 | 修复后 |
| ---: | ---: | ---: |
| 10,000 | 0.51s | 0.17s |
| 20,000 | 2.20s | 0.46s |
| 40,000 | 7.89s | 0.82s |
| 80,000 | 32.89s | 1.70s |

修复方式：删除全文件回扫，改为记录"发生过删除、可能仍有空位的页"的候选集，
插入时最多探测 8 个候选页，否则追加新页。空间回收能力保留，但代价有界。

这个问题值得记录：功能用例全绿、重启一致，只有把数据量拉到万级以上才暴露，
说明"正确性通过"不等于"规模可用"。

### 计划（未执行）

- M2：查询执行细化（表达式、TopK 语义边界、外排排序评估）。
- M3：WAL + checkpoint，覆盖 kill -9；顺带把每批提交的多余 fsync 合并。
- M4：HNSW 与召回校准。
