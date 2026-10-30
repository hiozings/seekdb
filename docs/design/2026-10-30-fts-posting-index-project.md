# FTS posting index 独立工程立项

## 1. 背景与问题

当前 FTS 索引的辅助表 `__idx_<data>_<index>` 的 rowkey 是
`(word_segment, doc_id)`，其中 `word_segment` 是 `WORD_SEGMENT()` 对原始列的
直接拼接（不是 token）。真正的 word posting 在多值索引/存储层。构建链路因此
必须按整段 `word_segment` 对 20k 文档排序：

- 去掉 DDL `ORDER BY`：`ERROR 4016 Internal error`；
- 改成 `ORDER BY doc_id`：`ERROR 4105 Rowkey order error`；
- 只改辅助表键序或位置：命中归零 / 主键重复。

在 DAS/DDL 层做补丁无法绕开这个行键约束，构建类 20%～30% 的目标无法达成。
需要新增独立的 word 级 posting index。

## 2. 目标与非目标

### 目标

1. 新增 FTS posting index type 及辅助表 schema，rowkey 为 `(word, doc_id)`；
2. DDL 构建直接按词生成 posting 行，按 `(word, doc_id)` 排序，不按整段
   `word_segment` 排序；
3. 查询 `MATCH` 按 word 扫描 posting 表，命中与相关性结果与现有实现一致；
4. 官方 FTS 基准构建类耗时相对 `0c3bf906c` 同机下降 20%～30%；
5. 保持官方查询命中数与 `test_fts_hotpath`/结果对照不变。

### 非目标（首期）

- 不替换 vector/multivalue/spatial index；
- 不改变官方 benchmark、scoring、baseline、测试文件；
- 不做全局/分区 FTS 的完整支持（首期只做 local IK/BENG 索引）；
- 不引入 benchmark 特判、硬编码或查询结果缓存。

## 3. 架构草案

### 3.1 Schema

新增 index type（暂名 `INDEX_TYPE_FTS_POSTING_LOCAL`）及辅助表：

```text
rowkey: (word, doc_id)
word:        VARCHAR, parser token, collation 与源列一致
doc_id:      VARCHAR binary
store:       word_count int, doc_length int
```

与现有 `__idx_..._fti_*`、`_fts_doc_word` 并存或替换由 M1 决策；首期建议并存，
通过新 index type 的 feature flag 切换，便于回滚。

### 3.2 构建

1. 数据表 scan 时逐篇文档调用现有 `ObFTParseHelper`/parser；
2. 生成 `(word, doc_id, word_count, doc_length)` 行；
3. 按 `(word, doc_id)` 排序后写 posting 表；
4. 保持现有 DDL task/并行框架，不改变外部 SQL 语法。

### 3.3 查询

1. `MATCH` 查询词分词；
2. 在 posting 表按 word 做等值/前缀扫描，取 doc_id 集合；
3. 相关性分数使用 posting 行的 `word_count`/`doc_length` 或 doc 表；
4. 与现有 FTS 多值索引解耦；M3 需保持命中 ID、排序和分数与旧实现一致。

### 3.4 关键挂载点（待 M0 确认）

- index type/schema：`src/share/schema/ob_schema_struct.h`、`src/sql/resolver/ddl/ob_fts_index_builder_util.cpp`；
- DDL 构建：`src/rootserver/ddl_task/ob_fts_index_build_task.cpp`、`src/rootserver/ob_index_builder.cpp`、`src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp`；
- DML/行生成：`src/sql/das/ob_das_domain_utils.cpp`（`generate_fulltext_word_rows`、`ObFTDMLIterator`、`ObFTIndexRowCache`）；
- 查询扫描：`src/sql/das/ob_das_domain_utils.cpp`、`src/sql/engine/table/ob_table_scan_op.cpp` 的 FTS 路径；
- 测试：`unittest/storage/fts/test_fts_hotpath.cpp`、`unittest/storage/fts/compare_fts_results.py`、官方 benchmark。

## 4. 里程碑与验收标准

### M0：设计验证（1～2 周）

交付：

- 确认新 index type/aux schema/DML/查询的实际挂载点与改动文件清单；
- 内存原型：对 20k 文档分词并生成 posting 行，统计 token 数/词频/doc_len；
- 与现有辅助表数据对照，确认 posting 内容可表示同样的倒排信息。

验收：

- 原型 posting 行数/词频与现有 parser 结果一致；
- 挂载点清单经过代码走查，无官方文件改动；
- 明确 M1 的 schema 与生命周期方案。

### M1：Schema 与 DDL 创建（2～3 周）

交付：

- 新 index type 枚举、aux schema、create/drop 生命周期；
- 建表后 rowkey/列/collation 符合设计；
- feature flag 默认关闭。

验收：

- `CREATE/DROP/重建` 不残留、不破坏旧 FTS 索引；
- schema 单元测试通过；
- 官方测试文件未改。

### M2：DML/插入计划（2～3 周）

交付：

- 逐词 posting 行生成与 `(word, doc_id)` 排序插入；
- 并行构建、错误处理、内存上限；
- 构建后 posting 表统计正确。

验收：

- posting 行数与 parser token 总数一致；
- 同文档词频、doc_len 与旧路径一致；
- 不在插入前按整段 `word_segment` 排序；
- 构建类同机 A/B 达到 ≥15%（阶段目标）。

### M3：查询扫描（2～3 周）

交付：

- `MATCH` 走 posting 表扫描；
- 命中集合、相关性分数、排序与旧路径一致；
- 开关可回退旧索引。

验收：

- 官方 4 条查询命中数 `8001/11000/7332/20`；
- 193 项结果对照（token/词频/doc_len/匹配 ID/分数/排序）一致；
- `test_fts_hotpath` 通过；
- 查询耗时相对旧路径不退化 >5%。

### M4：性能与收口（1～2 周）

交付：

- 同机 6 轮以上 paired A/B；
- 构建类 20%～30% 下降；
- 文档、回滚方案、已知限制。

验收：

- `build_ik_all/build_ik_content/build_beng/build_total` 中至少构建类平均
  ≥20%，目标 25%～30%；
- 分词/查询类不退化 >5%；
- 官方基准/评分/测试文件零改动；
- 无硬编码、无查询结果缓存；
- 可一键回退旧 FTS 路径。

## 5. 风险与回滚

- 查询语义/BM25 分数不一致：M3 以 193 项对照为硬门禁；
- posting 表存储/写入放大：M2 统计行数与空间，必要时压缩/分片；
- DDL 并行/恢复：沿用现有 task 框架，feature flag 默认关闭；
- 性能不达 20%：M2/M4 分阶段门禁，未达标不替换旧路径。

## 6. 当前状态

- 分支：`feature/fts-structural-rewrite`；
- 已完成的只是读/写位置接口与 schema 事实记录，没有 posting 表代码；
- 本立项文档提交后，从 M0 开始。

## 7. M0 结果（2026-10-30）

原型脚本：`unittest/storage/fts/posting_prototype.py`（只读 benchmark 数据库，
通过引擎 `TOKENIZE` 生成 posting，不修改官方文件）。

对 20000 行 benchmark 文档（title + content，IK parser）实测：

| 指标 | 值 |
|---|---:|
| 文档数 | 20000 |
| zero-token 文档 | 0 |
| 总 token 数 | 813664 |
| posting 行数（每文档去重词数求和） | 813664 |
| 唯一词数 | 26858 |
| 平均 tokens/doc | 40.683 |
| p50 / p95 / max tokens/doc | 41 / 48 / 50 |
| 解析错误 | 0 |

高频词示例：`row`、`文档`、`是`、`the`、`的`。

结论：IK all 索引约 81 万条 posting 行，word 键短、行数可控，`(word, doc_id)`
posting 表在容量上可行；M1 可以按该规模设计 schema 与写入批次。

M0 挂载点走查确认：

- index type/schema：`src/share/schema/ob_schema_struct.h`、`src/sql/resolver/ddl/ob_fts_index_builder_util.cpp`；
- DDL 构建：`src/rootserver/ddl_task/ob_fts_index_build_task.cpp`、`src/rootserver/ob_index_builder.cpp`、`src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp`；
- DML/行生成：`src/sql/das/ob_das_domain_utils.cpp`；
- 查询：`src/sql/das/ob_das_domain_utils.cpp`、`src/sql/engine/table/ob_table_scan_op.cpp`；
- 测试：`unittest/storage/fts/` 下自建工具、`test_fts_hotpath`。

M0 验收：posting 行数/词频由同一 parser 生成，统计与脚本一致；未改官方文件。

## 8. M1.2 schema 可行性检查（2026-10-30）

用 SQL 在真实 seekdb 实例上创建原型表：

```sql
CREATE TABLE __fts_posting_proto (
  word VARCHAR(1024) COLLATE utf8mb4_general_ci NOT NULL,
  doc_id VARCHAR(64) BINARY NOT NULL,
  word_count INT NOT NULL,
  doc_length INT NOT NULL,
  PRIMARY KEY (word, doc_id)
);
```

结果：

- 建表成功，`SHOW CREATE TABLE` 显示 `PRIMARY KEY (word, doc_id)`；
- `word` 为 `varchar(1024)`（general_ci），`doc_id` 为 `varchar(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin`；
- 插入 3 行成功，`SELECT COUNT(*) = 3`；
- 测试后已 DROP，不残留。

脚本：`unittest/storage/fts/posting_schema_prototype.py`。

结论：表层面的 schema/rowkey 可行。但把它作为 FTS aux 表自动创建仍不能复用现有
index-arg/generated-column 路径，因为数据表没有逐词列；需要专用的内部表创建
路径/lifecycle（下一步）。

## 9. M1.3 create/drop 生命周期设计（2026-10-30）

posting aux 表不能复用 `append_fts_index_arg` 的 generated-column/index-arg
路径，因为数据表没有逐词列。M1.3 采用**专用内部表创建路径**：

### 9.1 Feature flag

- 新增 hidden config `_enable_fts_posting_index`，默认 `false`；
- 所有 posting 表的创建、构建、查询、drop 都由该 flag 门控；
- flag 关闭时，现有 FTS 索引路径零改动。

### 9.2 创建

在 `ObFtsIndexBuildTask` 创建完现有 aux 表后：

1. 用 `build_fts_posting_schema()` 构造显式 `ObTableSchema`：
   - `word VARCHAR`（parser token，collation 与源列一致）
   - `doc_id VARCHAR BINARY`
   - `word_count INT`、`doc_length INT`
   - rowkey `(word, doc_id)`、index type `INDEX_TYPE_FTS_POSTING_LOCAL`
2. 通过 DDL service 创建内部表，表名 `__idx_<data>_<index>_fts_posting`；
3. 记录 table_id 到 FTS DDL task，用于失败清理和 drop；
4. 建表失败时回滚/删除，不留下孤立表。

### 9.3 Drop / rebuild

- FTS index drop task 在 flag 打开时先/后 drop posting 表；
- rebuild 先 drop 旧 posting 表再创建新表，避免残留；
- DDL task 重试要幂等：存在则复用或先 drop。

### 9.4 验收（M1.3）

- flag=false：现有 FTS 索引创建/查询/drop 行为完全不变；
- flag=true：创建一个空的 posting aux 表，`rowkey/列/collation` 与设计一致；
- `CREATE/DROP/重建` 后 `__all_table` 无残留 posting 表；
- 不影响现有 FTS 索引、查询命中数和 `test_fts_hotpath`；
- 官方评分/基准/测试文件零改动。

### 9.5 风险

- DDL scheduler 的生命周期/事务/重试比普通 SQL 建表复杂；
- 内部表需要纳入 FTS task 的失败清理路径；
- 专用建表路径需要新增 DDL service 调用，不能只靠 index-arg。

该设计确认了实现边界：M1.3 需要改 `ObFtsIndexBuildTask`/drop task 与 DDL
service 的建表入口，而不是此前的 SQL/DAS 位置管线。
