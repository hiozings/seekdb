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

## 2026-11-09（M3：WAL 与崩溃恢复）

### 崩溃模型与由此得到的设计约束

赛题把崩溃限定为"进程被强制终止"，不模拟断电或磁盘损坏。这允许两点简化：
页写入不加密钥校验和；恢复只需 redo，不需要 undo——前提是**未提交的页绝不能被写进数据文件**。
因此采用"no-steal + 提交时整页镜像日志"：

- 页缓存记录每页的属主事务号，换出时跳过属主非 0 的页；
- 提交时把本事务脏页的整页镜像写进 WAL，追加 COMMIT 记录并 fdatasync；
- 磁盘上只可能出现已提交的数据，恢复只重放有 COMMIT 的事务。

### 实施与验证

1. 新增 `src/txn/wal.{h,cpp}`（整页镜像 + CRC32 + 纪元号）、`src/util/crc32.h`。
2. 页头统一把 LSN 放在偏移 8（槽页与 B+tree 节点共用），重放时据此判幂等。
3. `HeapFile`/`BTree` 把事务属主与脏页收集透传给页缓存。
4. 新增 `tests/crash/kill_test.py`，每个 trial 覆盖四类窗口：

   - 自动提交语句逐条确认后随机点 kill → 已确认的行必须全部存在；
   - `BEGIN` 后插入 50 行不提交就 kill → 一行都不能出现；
   - `COMMIT` 确认后立即 kill → 已提交的行必须存在；
   - 发送 `COMMIT` 但不读确认、随机毫秒后 kill → 允许全有或全无，**不允许部分生效**；
   - 连续 3 次在恢复途中 kill，再次启动后必须收敛且不产生重复行。

   ```
   kill_test: 8 trial(s) passed in 0.7s
   ```

5. 回归：一致性用例 7/7、`btree_test` 全部通过；
   10 万行 × 128 维（51.9 MiB / 2 MiB 页缓存）：插入 2.18s、验证 0.10s、峰值内存 9.6 MiB
   （M1b 无 WAL 时插入 1.84s，WAL 带来的双写代价约 18%）。

### 发现并修复的三个缺陷

1. **B+tree 元数据页与页 LSN 冲突**：元数据页把 `version/root` 写在偏移 8–15，
   而 M3 统一把页 LSN 也放在偏移 8。提交时写 LSN 覆盖了元数据，重启后读到
   `root=65536` 的垃圾值，遍历内部节点时越界——由 ASAN 定位到
   `BTree::insert_rec → internal_key` 的 heap-buffer-overflow。
   修复：元数据字段整体后移到 16 字节之后，8–15 留给 LSN。
2. **WAL 记录只写了头、没写 payload**：`build_record` 计算 CRC 时读到了 payload，
   但构造字符串时忘记 append。现象是每页只写入 40 字节，页镜像全部丢失，
   崩溃后数据为空。修复后单条记录从 40 字节变为 8232 字节，
   崩溃恢复立即通过。
3. **索引元数据不可读时的自愈**：旧版本数据目录的元数据页版本号不匹配，
   读取时重置为"未构建"，引擎会从堆文件重建索引，避免带着坏索引启动。

这三个缺陷都没有被"正常退出再启动"的用例覆盖：第 1 个只在崩溃重放时触发，
第 2 个只在真正 kill 后触发。这说明**崩溃测试必须真的 kill，而不是优雅关闭**。

### 计划（未执行）

- M5：周期性 checkpoint（限制 WAL 增长）、行级逻辑日志（压缩写放大）、
  SIMD 距离核、过滤后 TopK 的索引路径。
- M4：HNSW 与召回校准。

## 2026-11-09（M4：HNSW 向量索引与召回校准）

### 决策

| 决策 | 理由 |
| --- | --- |
| 自研 HNSW，M=32、efConstruction=200 | 不引第三方索引库；M=16 在均匀随机 128 维上召回到不了 0.95 |
| 邻居选择用 HNSW 的多样性启发式，而非最近 M 个 | 最近 M 个会形成聚簇图，规模一大召回就塌 |
| 搜索从 ef=256 起，不足 k 条才翻倍 | "凑够 k 就停"会在召回路线上悄悄失效，必须设质量下限 |
| 带过滤时按实测通过率放大 ef（≈1/选择性） | 搜索先行的语义下，Accepted 集合过浅是过滤召回的主因 |
| 结果用存储向量重算精确距离并重排 | 图返回的是近似距离，直接输出会让距离与记录不一致 |
| 索引在打开时重建，回滚后标记陈旧再重建 | 图是派生状态，重建简单且正确；重建完成前不报就绪 |

### 实施与验证

1. 新增 `src/vector/hnsw.{h,cpp}`、`tests/storage/hnsw_test.cpp`（与暴力结果对照）。
2. `CREATE VECTOR INDEX` 与 `SET mode = 'exact'|'ann'` 接入解析器与引擎；
   catalog 升到版本 2 记录索引定义（列号），打开时据此重建。
3. 写入/更新/删除同步维护图；未提交事务的页不会落盘，回滚后标记陈旧。

### 召回优化过程（每步都是实测，不是估计）

| 配置（10k × 128 均匀随机，k=10） | 无过滤召回 |
| --- | ---: |
| M=16，ef=64 | 0.5967 |
| M=16，ef=256 | 0.8500 |
| M=32，ef=64 | 0.7917 |
| M=32，ef=128 | 0.9100 |
| M=32，ef=256 | 0.9683 |
| M=32，ef=256 + 过滤时按通过率放大 ef（120 查询） | 0.9992（无过滤）/ 0.9892（10% 选择性） |

关键结论：**均匀随机 128 维是 HNSW 的困难负载**。同一实现在 5000 行 × 32 维上
ef=64 就有 0.909，到 10000 行 × 128 维只有 0.59——参数不能按小规模实验拍板。

### 发现并修复的缺陷

**悬空引用**：`build_vector_index` 里写成
`add(pk, row[vec_col].v, std::move(row))`，而 C++ 未规定实参求值顺序；
`std::move(row)` 先于读取 `row[vec_col].v` 执行时，引用立即悬空。
ASAN 报在 `VectorIndex::add` 的 `node.vec = vec`。
修复：先把向量拷贝到局部变量，再移动整行。

### 计划（未执行）

- M5：索引持久化以缩短启动/重建时间；SIMD 距离核；查询延迟的精确分解
  （规划 / 图遍历 / 重排 / 输出）；周期性 checkpoint 限制 WAL 增长。

## 2026-11-09（M5：性能工程）

### 先建立可测量的基线

给引擎加了 `--stats`：输出各阶段累计耗时（扫描 / ann / 提交 / 索引构建）、
扫描行数、平均 ef、峰值 RSS 与进程块 I/O。没有这些数字，前面几轮的"优化"只能靠猜。

首次基线（10000 行 × 128 维，400 次查询）：

```
load 4.667s（其中 index_build 4.29s）
queries 0.721s（1.80 ms/query，avg_ef=1024）
```

### 逐个优化与实测效果

| 手段 | 前后 |
| --- | --- |
| AVX2 距离核（`l2_rank` 供图遍历，`l2_exact` 供输出） | 索引构建 4.29s → 1.55s；查询 1.80 → 1.44 ms |
| 过滤条件预解析（列下标 + 已强转字面量，避免每行做字符串查找） | 1.44 → 1.10 ms |
| 无分配 Top-K 扫描（按行字节定位向量列，只解码最终 k 行） | 精确查询 1.5 → 0.34 ms |
| 页缓存 256 → 2048 页 | 同上（此前每扫描一次就把 5 MB 数据换入换出） |
| 向量索引改为首次 ann 查询时惰性构建 | 打开 1.7s → 2ms |

最终（1000 次查询，exact，不建索引）：

```
load 0.303s（commit 0.06s）   queries 0.356s（0.36 ms/query）   peak RSS 11.4 MiB
```

### 两个反直觉的结论

1. **在这个规模上 HNSW 比精确扫描慢。** ef=1024 时一次查询要访问约 1024 个节点、
   每个节点 32 条边，合计约 3.3 万次距离计算；而全表扫描只有 1 万次。
   实测 ann 约 0.6–0.8 ms/查询，exact 0.36 ms/查询。因此正式提交固定 `exact`，
   HNSW 保留给近似检索用例与更大数据集。
2. **真正的瓶颈不是距离计算，而是页缓存容量和行解码。** 页缓存只有 2 MiB 时，
   每次扫描都要把 5 MB 数据换入换出（且脏页还要回写），单查询 1.5 ms；
   提到 16 MiB 后直接降到 0.34 ms——比换 SIMD 的收益还大。

### 发现并修复的缺陷

**惰性构建导致 ann 静默退化**：把索引构建改为惰性后，`ann_eligible` 仍然要求
`table.vec_index` 非空，于是重启后的 ann 查询全部悄悄走了精确扫描。
召回校准脚本因此报出"ann 召回 1.0000"这种反常结果，才被发现。
修复：资格判断改用 catalog 里的索引定义（`meta.has_vector_index`），
实际图在首次 ann 查询时构建。

这个缺陷的教训是：**性能优化改变了控制流，必须用质量指标回看**，
否则"召回 100%"会被当成好消息。

### 计划（未执行）

- 向量列做列式缓存（连续 float 数组）以进一步贴近内存带宽上限。
- 索引持久化为独立工件，去掉首次 ann 查询的 1.9 s 重建。
- 写路径：行级逻辑日志替代整页镜像，压缩 WAL 写放大。

## 2026-11-09（线上准入失败后的兼容性加固）

### 事实

线上反馈："MySQL 正式用例 未通过；此项为准入条件，不计排名分"。官方题包与测试驱动未公开，
本地 10 个用例是自建 golden，因此失败原因只能从协议兼容面反推。

### 排查：驱动常见的"非测试语句"

用真实语句逐条探测修复前的行为，发现多条会直接报错——只要驱动在连接或收尾阶段发送其中
任意一条，整个用例就会被判失败：

```
SELECT 1;                       -> ERROR 1064: syntax error: expected a column name
SET NAMES utf8mb4;              -> ERROR 1235: only SET mode = 'exact'|'ann' is supported
SET autocommit=1;               -> ERROR 1235
USE test;                       -> ERROR 1064: unsupported statement 'USE'
CREATE TABLE IF NOT EXISTS ...  -> ERROR 1235: IF NOT EXISTS is not supported
DROP TABLE IF EXISTS t;         -> ERROR 1064: unsupported statement 'DROP'
```

### 修复

1. `SELECT <字面量>`（无 FROM）：支持 `SELECT 1`、`SELECT 1 AS one, 'x' AS label`。
2. 会话类 `SET`（`SET NAMES`、`SET autocommit`、`SET SESSION ...`）按无操作接受；
   `SET mode = 'exact'|'ann'` 仍然生效。
3. `USE <db>` 接受为无操作（引擎只有单一数据目录）。
4. `CREATE TABLE IF NOT EXISTS` 与 `DROP TABLE [IF EXISTS]` 真正实现；`DROP` 在事务内拒绝。
5. 语句终止兼容：除 `;` 外，**单行**且能被完整解析的缓冲区也会立即执行，
   以适配"每行一条语句、不带分号"的驱动；多行语句仍依赖 `;`，不会被提前执行。

解析器新增 `kIncomplete` 内部错误码（不会打印），用于区分"语句还没写完"和"语句有语法错误"，
这是单行回退判断的依据。

### 验证

```
10/10 conformance（新增 m6_compat.test 覆盖上述兼容语句）
btree_test / hnsw_test / kill_test / 2 万行大数据集 全部通过
```

### 仍未解决

官方驱动未到手，输出格式（表头、分隔符、浮点格式、DML 输出）、错误码文本、
就绪标识与 exact/ann 切换方式仍无法对账。这些旋钮集中在 `FormatSpec`，
拿到题包或预检日志后应可在一小时内调整。

## 2026-11-09（准入失败的真正原因：协议是 JSON，不是文本）

### 发现

线上连续两次报"MySQL 正式用例 未通过"。此前一直假设题包不在仓库里，直到重新
`git fetch upstream` 发现 **`2026-competition-round2` 分支已经前进**
（`c842a368c` → `9f6df2ba5`），组织方上传了 `mysqltest/`：

```
mysqltest/README.md          协议与断言规则
mysqltest/manifest.json      20 个公开样例
mysqltest/cases/*.test       每个非空行一条 SQL 或驱动指令
mysqltest/cases/*.result     JSON 数组，与 SQL 一一对应
```

真实协议与我们实现的完全不同：**每条 SQL 输出一个 JSON 对象**（一行一个），
而不是制表符分隔的文本。

| 语句 | 期望输出 |
| --- | --- |
| CREATE TABLE / BEGIN / COMMIT / ROLLBACK / CREATE VECTOR INDEX | `{}` |
| INSERT / UPDATE / DELETE | `{"affected_rows": N}` |
| SELECT | `{"columns": [...], "rows": [[...]]}`，空结果也带 `columns` |
| 失败 | `{"error": "<语义名>"}`，如 `duplicate_key`、`unsupported_sql`、`dimension_mismatch` |

此外 `--restart`、`--mode ann` 是驱动指令，不占断言位置。

### 修复

1. 输出层新增 JSON 模式并设为默认（`MVDB_FORMAT=text` 保留用于调试）。
2. `ResultSet` 增加列类型（`CellKind`），JSON 才不会把字符串数字当数字输出；
   浮点在 JSON 模式用 `%.17g`，BIGINT 原样输出（`9007199254740993` 精确）。
3. 错误改成语义名映射（重复主键 → `duplicate_key`、不支持语句 → `unsupported_sql`、
   维度不符 → `dimension_mismatch` 等）。
4. 支持驱动指令：`--restart` 回滚未提交事务并退出，`--mode ann|exact` 切换模式。
5. 把官方 20 个样例复制进 `tests/official/`，并写了按官方协议驱动的 runner。

### 验证

```
20/20 official cases passed      # 官方公开样例
10/10 自建 conformance（已改为 JSON 期望）
kill -9 / B+tree / HNSW / 2 万行大数据集 全部通过
```

### 教训

连续两次失败后，正确动作不是继续猜协议，而是**回头确认上游分支是否更新**。
组织方的题包一直放在比赛分支上，只是我们从未重新 fetch；一次 `git fetch` 比两轮猜测更有价值。

## 2026-11-10（性能优化第二轮：列式扫描缓存 + 解析器）

### 先量再改

用微基准确定扫描的理论下限（10000 行 × 128 维、连续布局）：

```
contiguous exact: 0.157 ms/query  (32.6 GB/s, 15.7 ns/row)
contiguous rank : 0.117 ms/query  (43.7 GB/s, 11.7 ns/row)
```

而引擎实测 0.36 ms/查询——差距不在距离核，而在"走槽页 + 每行重算列偏移"。

### 改动

1. **列式扫描缓存**：首次 Top-K 查询时把向量按行连续拷贝（`N × dim` 的 float 数组），
   标量列另存进 arena 供过滤；任何写入都使缓存失效。超过 96 MiB 预算则不建缓存，
   回退到页扫描，保证"超过内存限额的数据集"仍可工作。
   扫描 0.36 s → 0.18 s；查询 0.40 → 0.24 ms。
2. **向量字面量就地解析**：原实现对每个元素 `split` 成 `std::string` 再 `trim`，
   一次批量导入要做上百万次分配；改为在 `string_view` 上用 `from_chars` 直接扫描。
   解析 60 → 40 ms。

### 修复的缺陷

列式缓存构建时用 `ids.back()` 写主键，但主键列在向量列之前时 `ids` 仍为空——
堆越界，退出时报 `free(): invalid size`。改为先收集主键到局部变量、整行校验通过后再提交。

### 结果（10000 行 × 128 维，JSON 协议）

| 指标 | 优化前 | 优化后 |
| --- | ---: | ---: |
| 查询（exact，1000 次） | 0.40 ms/次 | **0.24–0.25 ms/次** |
| 批量写入（100 批） | 0.30 s | **0.22 s** |
| 峰值内存 | 11.6 MiB | 11.5 MiB |

写入侧剩余成本：提交 fsync 58–95 ms、解析 38–61 ms、B+tree 插入 4–7 ms，
已接近该配置下的下限（每批一次 fsync 是持久性要求）。

回归：官方 20 例、自建 10 例、kill -9、B+tree、10 万行大数据集全部通过。

## 2026-11-10（词法零拷贝）

### 改动

- `Token.text` 由 `std::string` 改为 `std::string_view`，指向语句缓冲区；
  只有含 `''` 转义的字符串字面量才在 `std::deque<std::string>` 里物化一份
  （deque 保证元素地址稳定，移动后视图仍有效）。
- `StatementSplitter` 返回 `string_view` 而不是整段拷贝，并用 `start_` 标记已消费前缀，
  超过 64 KiB 才压缩一次。
- `handle_statement` 用 `trim_view`，去掉每条语句的一次全量拷贝。

### A/B 实测（10000 行 × 128 维，16 MB SQL，各 4 次取中位）

| 版本 | parse_ms |
| --- | ---: |
| e438e24（拷贝版） | 41.4 |
| 本次（零拷贝） | **35.1** |

查询与写入的端到端数字在噪声范围内（提交 fsync 主导写入，扫描主导查询），
因此这次改动的价值主要是去掉 16 MB 级别的无效拷贝。

### 过程中引入并修掉的缺陷

切分器在**返回视图之后**立刻调用 `compact()`，把缓冲区前移，视图随之失效——
10 万字节以上的多行 INSERT 全部变成乱码（`syntax error: unexpected character '\0'`），
表现却是"插入失败但程序不报错退出"，基准里只体现为 commit=0、耗时骤降。
修复：压缩只在 `next()` 入口、上一视图已被消费后执行。

新增 `m7_large_statement` 回归用例（82 KB 单条语句），防止再次退化。

## 2026-11-10（过滤位图）

### 设计

基准里 500 条过滤查询只涉及 10 个 `category` 值。为单等值条件建立
(列, 值) → 位图：首次使用时对扫描缓存扫一遍，把每个取值对应的行位置进位图；
查询时只遍历置位的行，未命中的值直接返回空结果。位图与扫描缓存同生共死，
写入即失效，因此不可能读到陈旧数据。

只对"恰好一个等值条件"启用；其他形态回退到逐行判断，不改变语义。

### A/B（10000 行 × 128 维、1000 次查询、各 5 次取最小）

| 版本 | 每查询耗时 | 扫描行数 |
| --- | ---: | ---: |
| 15498b4（无位图） | 0.233 ms | 10,000,000 |
| 本次（位图） | **0.167 ms** | 5,500,000 |

扫描行数正好是 500×10000（无过滤）+ 500×1000（过滤命中 10%），
说明位图路径确实生效，而不是"碰巧快了"。

### 回归

官方 20 例、自建 11 例、kill -9、B+tree、10 万行数据集全部通过。

### 剩余空间

过滤查询已接近零成本，无过滤查询成了主导项（500 × 0.157 ms ≈ 78 ms，占 167 ms 的一半）。
再往下只能靠"float32 预选 + 精确重排"，属于用正确性换速度，暂不做。
