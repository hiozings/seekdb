# FTS 紧凑 posting 索引（C 系列）

## 1. 背景

现有 FTS 索引把每个 `(word, doc_id)` 存成一行 row-store posting：

- 20k 文档 IK all：813,664 行；content：693,336 行；BENG：381,900 行；
- 构建 813k 行的写入/排序是 build 的主要成本；
- 查询按 token 扫描全部 posting（单 token 约 106ns/posting，2/3 token 约
  223/285ns/posting），耗时随 posting 总数线性增长。

## 2. 原型验证（2026-10-30）

把每个 token 聚合成一行：`word -> 有序 (doc_id, tf, doc_len) 数组`，
用普通表 + BLOB 存储，20k 文档实测：

| 表 | word 数 | posting 数 |
|---|---:|---:|
| cp_all（title+content, ik） | 26,858 | 813,664 |
| cp_content（content, ik） | 20,150 | 693,336 |
| cp_beng（title_en+content_en, beng） | 19,965 | 381,900 |

并集命中与官方完全一致：

| 查询 | 扫描 posting | 并集 | 官方命中 |
|---|---:|---:|---:|
| 全文索引 分词器 | 12,669 | 8001 | 8001 |
| database performance audit | 18,000 | 11000 | 11000 |
| OceanBase 稳定 database | 12,665 | 7332 | 7332 |
| 倒排索引 tokenizer | 14,000 | 9333 | LIMIT 20 |

查询耗时（200 次平均，只读取 compact blob 并求长度，未做客户端解码）：

| 查询 | compact 读取 | 当前 MATCH | 倍数 |
|---|---:|---:|---:|
| cn | 170µs | 3915µs | 23× |
| beng | 190µs | 4724µs | 25× |
| mixed | 153µs | 4054µs | 26× |
| limit | 197µs | 4737µs | 24× |

加上 C++ 端解码/并集（预计 50～150µs），查询仍有 10 倍以上空间。

## 3. Schema

```sql
CREATE TABLE `__fts_compact_<data_table_id>_<aux_table_id>` (
  word      VARCHAR(1024) COLLATE utf8mb4_general_ci NOT NULL,
  docs      LONGBLOB NOT NULL,
  doc_count INT NOT NULL,
  PRIMARY KEY (word)
);
```

`docs` 为按 doc_id 升序的 16 字节记录数组：

```
struct Posting { uint64 doc_id; uint32 tf; uint32 doc_len; }
```

词级全局统计（doc_freq = doc_count、total_doc_cnt、sum_doc_len）用于
精确复现 BM25。

## 4. 构建集成

### C1：schema 与构建

- 复用 `_enable_fts_posting_index`/新 flag 门控；
- 在现有 FTS DDL 的 domain aux 构建过程中，从
  `ObFTIndexRowCache::segment` / `generate_fulltext_word_rows` 收集
  `(word, doc_id, tf, doc_len)`；按 index 维度分片聚合，避免重复分词；
- 父任务在 aux 构建完成后，把聚合结果（约 26k 行）批量写入 compact 表；
- 跳过 `fts_doc_word` 的数据构建（第二次分词），必要时保留空表兼容；
- 失败/重试时清理 compact 表，flag 关闭时完全走旧路径。

### C2：查询

- 在 `ObTableScanOp` 的 TEXT RETRIEVAL SCAN 路径检查 compact 表；
- 用查询 token 做 `word IN (...)` 点查，读取 2～3 个 blob；
- C++ 端归并有序 doc_id 数组，计算 BM25（tf、doc_len、doc_freq、
  total_doc_cnt、avg_doc_len 与 `ObExprBM25` 一致），支持 COUNT/LIMIT；
- compact 不可用时回退现有 DAS 路径，保证正确性。

### C3：DML

- INSERT/UPDATE/DELETE 需要维护 compact blob；
- 首期可以采用“DML 时失效 compact、查询回退旧路径”的保守方案，
  但要求旧 aux 表仍然可维护；
- 若跳过旧 aux 数据构建，则必须实现 blob 的读-改-写更新。

### C4：性能与收口

- build 目标：去掉 813k 行 row-store posting 写入，20k 文档构建从
  8.6s 降到 3～4s；
- query 目标：0.2～0.5ms；
- tokenize 保持现有共享词典/parser 优化；
- 三类综合目标 ≥50%。

## 5. 风险

- DDL 并行收集的线程安全与内存上限（813k posting ≈ 50MB/索引）；
- 查询 BM25 与旧实现的逐值一致性（193 项对照为门禁）；
- DML 维护复杂，首期需要 flag 回退；
- compact 表不可用时必须可靠回退。

## 6. C1a 实施结果（2026-10-30）

已完成：

- `src/storage/fts/ob_fts_compact_posting.h`：16 字节 posting 编解码、
  doc_id 有序数组归并计数；`test_fts_hotpath` 增加 `CompactPostingCodec`
  用例并通过；
- `ObFtsIndexBuildTask::create_posting_aux_table()` 的 aux 表 schema 改为
  `(word varchar(1024) PK, docs longblob, doc_count int)`；
  flag 打开、2 万行构建后，`__all_column` 实测三张表均为
  `word`(rowkey) / `docs`(longblob) / `doc_count`(int)；
- flag 关闭时行为不变。

关键负结果：**内层 SQL 无法 SELECT 隐藏 aux 表**。在 FTS DDL task 内用
`GCTX.sql_proxy_->read()` 执行
`SELECT 1 FROM db.__idx_... LIMIT 1` 返回 `-5019`（bad field），
`SELECT COUNT(1)` 同样失败；因此不能通过“扫描 domain aux”来生成
compact posting，必须在 DDL 构建的 scan/insert 执行过程中收集 word 行。

下一步 C1b：

- 在 `ObPxMultiPartSSTableInsertOp` / scan op 的 FTS DDL 路径中，
  按目标 aux table id 把 `(word, doc_id, tf, doc_len)` 收集到进程级
  sharded collector；
- 父任务成功后按 word 聚合、排序、编码并批量写入 compact 表；
- 失败/重试时清理 collector 与 compact 表。

## 7. C1b 实施结果（2026-10-30）

在 FTS DDL 的 scan 行生成路径（`ObTableScanOp::inner_get_next_fts_index_row`）
按 DDL child task id 收集 `(word, doc_id, tf, doc_len)`，父任务成功后
flush 到 compact 表。

关键实现：

- 引擎的 doc_id 是 **16 字节不透明索引键**（实测 `doc0=410d0300...`，
  前 8 字节为 tablet id 200001），不是 u64；compact posting 改为
  `(16B doc_id, tf, doc_len)` = 24 字节；
- `ObFtsCompactCollector` 按 16 shard 聚合，flush 时按 16 字节 doc_id
  排序、编码、批量 `INSERT`；
- `ObFtsCompactPostingCodec` 支持 24 字节 posting 的编码/解码/并集计数；
- flag 打开时创建 compact 表并收集；flag 关闭时完全走旧路径。

2 万行实测（flag 打开）：

| 索引 | compact words | compact postings |
|---|---:|---:|
| all/ik | 26,858 | 813,664 |
| content/ik | 20,150 | 693,336 |
| beng | 19,965 | 381,900 |

用 compact 表做并集，命中与官方一致：

| 查询 | 扫描 posting | 并集 | 官方 |
|---|---:|---:|---:|
| 全文索引 分词器 | 12,669 | 8001 | 8001 |
| database performance audit | 18,000 | 11000 | 11000 |
| OceanBase 稳定 database | 12,665 | 7332 | 7332 |
| 倒排索引 tokenizer | 14,000 | 9333 | LIMIT 20 |

当前构建耗时（仍同时构建旧 aux + compact）：13.3s vs 旧路径 8.6s，
属于预期回归；C3 将跳过 `fts_doc_word`/domain aux 的旧数据构建并优化
flush，把 compact 作为唯一 posting 路径。

下一步 C2：MATCH 查询读取 compact 表，按查询 token 做 `word IN (...)`
点查，24 字节 posting 归并、BM25 计算，compact 不可用时回退旧 DAS。

### C1b 清理后复测

移除调试输出、重新构建后，flag 打开的 2 万行构建仍然生成：

- all: 26858 words / 813664 postings
- content: 20150 words / 693336 postings
- beng: 19965 words / 381900 postings

构建 total 14.3s（旧路径 8.6s），回归来自“旧 aux 数据 + compact”双份构建
与 SQL flush；C3 将去掉旧 aux 数据构建并批量化 compact 写入。

## 8. C2a：compact 元数据注册（2026-10-30）

新增 `src/sql/das/ob_fts_compact_registry.h`：

- 以 FTS aux index table id 为 key，记录 compact 表 id、tablet id、
  schema version、word/docs/doc_count 列 id；
- FTS DDL `succ()` 在 flush 成功后从 schema 查出 compact 表并注册；
- 进程内有效；重启后注册丢失，查询自动回退旧 DAS 路径（正确性优先）。

验证：flag 打开构建后三张 compact 表仍为
26858/813664、20150/693336、19965/381900；`test_fts_hotpath` 6/6。

下一步 C2b：

- 在 `ObDASLegacyTRMergeIter` 中按 aux id 查 registry；
- 非 topk（COUNT 类）查询用 compact dim iter 读取 compact 行，
  解码 24 字节 posting，填充 doc_id/relevance；
- topk/LIMIT 与注册缺失时回退旧 DAS；
- relevance 首期取常数 1.0，仅用于 COUNT 命中；完整 BM25 在 C2c 补齐。

## 9. C2b：compact 查询路径（2026-10-30）

`ObDASLegacyTRMergeIter` 在 `daat_mode_ && !topk_mode_ && !function_lookup_mode_`
且 registry 命中时走 compact：

- `ObFtsCompactScanReader`：`ObTableParam` + `ObTableScanParam` +
  `ObAccessService::table_scan` 按 `word` 点查 compact 行，读取 `docs` blob；
- `ObFtsCompactDimIter`：解码 24 字节 posting，提供 doc_id/score（首期 1.0）；
- 跳过 DAS inv scan / children / BM25 estimation（`skip_bm25_estimation_`）。

实测（flag 打开，同一进程 build 后查询）：

| 查询 | 命中 | compact 延迟 |
|---|---:|---:|
| cn COUNT | 8001 | 3181µs |
| mixed COUNT | 7332 | 2802µs |
| beng COUNT | 11000 | 3741µs |
| LIMIT 20（回退旧路径） | 20 | 4230µs |

对比旧路径约 3.9～4.7ms，仅提升 15%～25%。原因：compact 只省掉倒排
存储扫描；剩余成本在通用 DAAT 逐行 merge/projection（12k～18k posting），
约 220～285ns/posting。要拿到 10 倍收益必须做 **bulk merge / count 下推**。

当前限制：compact 模式 relevance 固定 1.0；非 topk 但投影 MATCH 分值的
查询会得到错误分数，因此 flag 默认关闭。C2c 将：

- 按 token 计算精确 BM25（compact 表需存 total_doc_cnt/avg_doc_len，
  或由 registry 在构建后写入）；
- 对 `COUNT(*)` 做 bulk 归并计数，跳过逐行 projection；
- 仅在这些条件满足时启用 compact，其余全部回退旧 DAS。

## 10. C2c：bulk merge 与投影瓶颈（2026-10-31）

新增 `ObFtsCompactMergeIter`：一次性对 compact posting 做 k 路归并，
预计算匹配 doc_id/score，再按 batch 投影。

实测（flag 打开）：

| 查询 | 命中 | C2b | C2c |
|---|---:|---:|---:|
| cn COUNT | 8001 | 3181µs | 2329µs |
| mixed COUNT | 7332 | 2802µs | 2589µs |
| beng COUNT | 11000 | 3741µs | 3735µs |
| 0 token | 0 | 249µs | — |
| 1 token（4668 posting） | 4668 | 1326µs | — |

瓶颈已从“倒排存储扫描”转到**逐行 projection**：8k 匹配仍需逐行写
id/relevance 两个 output datum。尝试用 `locate_batch_datums` 批量写，
因 batch frame 假设不满足触发 SIGSEGV，已回退为逐行 guard 写法。

因此 query 仅提升约 20%，不足以抵消 flag 打开时构建 13s 的回归。
flag 保持默认关闭；关闭时默认路径构建 9.39s、命中 8001/7332/11000、
LIMIT 20 正确，`test_fts_hotpath` 6/6、`ctest -L competition` 9/9。

要拿到 query 的 10 倍收益，必须做 **COUNT 下推**：把 FTS scan 的匹配数
直接变成 aggregate 的计数，跳过 8k 行 projection。这需要改
optimizer/aggregate（例如识别 `COUNT(*) + MATCH` 并生成 count 算子），
不是本地 iter 能完成的。下一步优先级：

1. COUNT 下推（query 最大杠杆）；
2. compact-only 构建，去掉旧 aux（build 最大杠杆，但 topk 需先支持）；
3. 精确 BM25 写入 compact。

## 11. C2d：精确 BM25 与 topk 边界（2026-10-31）

compact 表构建时按 16 字节 doc_id 去重统计 total_doc_cnt / avg_doc_len，
写入 registry；`ObFtsCompactDimIter::score_at` 按引擎同款公式计算：

```
idf = log((max(total-df,0)+0.5)/(df+0.5))
token_weight = max(0.25, idf) * (1+k1), k1=1.2, b=0.75
doc_weight = tf / (tf + k1*(1-b+b*dl/avgdl))
```

非 topk 查询实测分数与旧路径**完全一致**（id 1/2/3：
1.595015523834721 / 1.5652540277062 / 0.3834794348806822）。

topk/LIMIT 尝试直接启用 compact：旧 topk 路径的分数本身与
非 topk 分数略有差异（如 3.894413 vs 3.894549），导致 top20 顺序
不一致；已回退为 topk 走旧路径。回退后 LIMIT top20 与旧路径逐行一致。

flag 打开最终实测：

- 构建 total 11.9s（旧 aux + compact 双份）；
- COUNT 命中 8001/7332/11000；
- cn COUNT 2.28ms、beng COUNT 3.82ms（旧约 3.9/4.7ms）；
- LIMIT top20 与旧路径一致；
- `test_fts_hotpath` 6/6、`ctest -L competition` 9/9。

结论：compact 已能正确服务非 topk 查询，但 query 收益仍约 20%～40%，
构建有回归；要把整体做到 50% 需要 COUNT 下推（optimizer/aggregate）
或 compact-only 构建（先补齐 topk 精确分数）。

## 12. COUNT 下推尝试（2026-10-31）

确认 COUNT 计划：

```
SCALAR GROUP BY  output T_FUN_COUNT(*)
└─ TEXT RETRIEVAL SCAN  output(nil), pushdown_match_filter(MATCH ...)
```

`output(nil)` 说明 scan 不向父算子输出列，于是实现了 projection 跳过链：

- `ObOperator::set_skip_projection()` 虚接口；
- `ObScalarAggregateOp` 在“只有一个 COUNT、无 distinct、child 是 table scan”时
  调用 `child_->set_skip_projection()`；
- `ObTableScanOp` 把 hint 传给 DAS iter tree；
- `ObDASLegacyTRMergeIter` → `ObFtsCompactMergeIter` 跳过 id/relevance datum 写入。

实测 COUNT 命中仍为 8001/7332/11000，但延迟几乎不变
（cn 2.64ms、mixed 3.01ms、beng 3.75ms）。原因：**projection 不是主要成本**；
aggregate 仍要逐行消费 8k 个匹配，compact merge 仍要预计算并输出 8k 行。

真正的 COUNT 下推需要一个“单行 count”物理算子（例如 `PHY_FTS_COUNT`）：
直接调用 DAS merge 的 `get_match_count()`，返回一行计数，跳过 8k 行输出。
这需要新增算子、factory/codegen、optimizer 规则与序列化，工作量大于本轮范围。

当前状态：flag 默认关闭；默认路径、测试均不变。

## 13. COUNT 真下推（2026-10-31）

在 aggregate 层新增真正的 count 快路径，而不是只跳过 projection：

- `ObOperator::get_match_count(int64_t&)`，默认 `OB_NOT_SUPPORTED`；
- `ObTableScanOp` 转发到 DAS iter tree；
- `ObDASLegacyTRMergeIter` 在 compact 模式下创建 dim iter / compact merge，
  调用 `ObFtsCompactMergeIter::get_match_count()`；
- compact merge 新增 `prepare(count_only=true)`：只做 k 路归并计数，
  不分配 result、**不计算 BM25**；
- `ObScalarAggregateOp::inner_get_next_row`：
  - 仅当 `COUNT(*)`（无 distinct、无参数）、child 是 table scan、
    compact count 可用时；
  - 直接 `group_row->aggr_cells_[0].set_row_count(match_count)` + `collect()`，
    跳过 8k 行 child 输出和 8k 次 `process`。

正确性：

- `COUNT(*)` = 8001 / 7332 / 11000；
- `COUNT(id)` = 8001（非空 id，未被下推）；
- `COUNT(NULL)` = 0；
- `COUNT(id)` 无 MATCH = 20000。

性能（server CPU，600 次查询）：

| 版本 | server ticks | 每查询 |
|---|---:|---:|
| compact 全量 projection | 约 4ms（历史） | 4.0ms |
| count 下推 + 计算 BM25 | 138 ticks | 2.30ms |
| count 下推 + 跳过 BM25 | **101 ticks** | **1.68ms** |

即 COUNT 路径约 **2.4 倍** CPU 提升；wall 受宿主噪声影响在 1.9～3.1ms 波动。
LIMIT 仍走旧路径。

下一步：

1. compact-only 构建（去掉旧 aux），需要先解决 topk 精确排序；
2. 或者继续优化 compact 读取（blob 拷贝、扫描）把 1.68ms 压到 ~1ms。
