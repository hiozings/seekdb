# FTS 辅助索引结构性重写：阶段 1 与阶段 2 计划

## 分支与提交

- 分支：`feature/fts-structural-rewrite`
- 阶段 1 提交：`6deea5d2a refactor(fts): 按列描述解析辅助表 word/doc 位置`

## 阶段 1：读路径位置解耦（已完成）

`ObDASDomainUtils` 原先在多处硬编码：

```cpp
DOC_ID_IDX = is_fts_index_aux ? 1 : 0;
WORD_SEGMENT_IDX = is_fts_index_aux ? 0 : 1;
```

阶段 1 的做法：

- 为 `ObDmlTableView` 增加 `get_col_descs()`，暴露计划中已序列化的列描述；
- 新增 `get_fts_word_doc_idx()`：按 typed doc-id 列 id 定位 doc_id，再用字符串列类型定位 word_segment，失败时回退到 binary/非 binary 字符串判断；
- 替换 `get_ft_and_doc_id`、`get_ft_and_doc_id_for_update`、`scan_ft_word_rows` 的固定下标。

当前 schema 下解析结果仍是 word=0、doc=1，行为不变。验证：

- 官方基准查询命中数：`8001 / 11000 / 7332 / 20`；
- `test_fts_hotpath` 5/5；
- 官方基准/评分/测试文件未改。

阶段 1 不改变构建性能，只是让后续键序/布局变更不再依赖固定下标。

## 阶段 2：word/doc 级构建与读路径

### 仍需修改的位置

1. **写路径位置**：
   - `ObDASDomainUtils::generate_fulltext_word_rows` 仍接收 `is_fts_index_aux` 并按 0/1 写行；
   - `ObFTDMLIterator::build_ft_word_row` 仍硬编码 `DOC_ID_IDX/WORD_SEGMENT_IDX`；
   - `ObFTIndexRowCache::init/segment` 仍按 `is_fts_index_aux_` 生成 0/1 行。
   这三处需要和读路径共用同一套位置解析，否则改键序后写入的数据会错位。

2. **DDL 辅助表键序**：
   - `ObFtsIndexBuilderUtil::adjust_fts_args` 的 `is_fts_index` 分支目前是 word 在前、doc 在后；
   - 需要改为 doc_id 在前，或直接生成 `(word, doc_id)` posting 行；
   - 改动后 DDL 的 `ORDER BY` 才会从整段 word_segment 排序变成整数 doc_id/短 word 排序。

3. **FTS 索引构建计划/多值索引**：
   - 仅改键序在早先实验中构建耗时没有下降：`8.795s` vs `8.892s`（噪声内），说明整段排序不是主要构建成本；
   - 若要拿到 20%～30%，必须进一步把构建改成 word 级 posting 行，避免按整段 word_segment 写入/排序，并同步调整查询扫描。

### 风险

- 写路径、读路径、DDL plan、多值索引必须同时一致，否则查询命中会归零（早先只改键序的实验已出现 `0/0/0/0`）；
- word 级 posting 行会改变辅助表的行数/布局，影响 FTS DDL 与查询扫描，回归面大；
- 宿主噪声足以淹没 5% 量级的差异，构建收益必须用同机多轮配对验证。

## 结论

阶段 1 已落地并通过命中/单测验证；阶段 2 是真正的结构性改动，涉及写路径、DDL 键序和多值索引。早先的 doc_id 优先实验在未修读路径时构建无收益且查询为 0，因此阶段 2 不能只做键序交换，必须同时做 word 级 posting 构建才可能达到 20%～30% 的构建目标。
