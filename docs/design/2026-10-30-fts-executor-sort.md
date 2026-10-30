# FTS 执行器/插入计划子工程 E1

## 1. 目标

在 token 行展开之后按 `(word, doc_id)` 输出有序行，去掉 DDL 对整段
`word_segment` 的预排序；默认 flag 关闭，可回退；评估构建 20%～30%。

## 2. 当前执行顺序

1. DDL SQL：`SELECT ... FROM docs ORDER BY word_segment, doc_id`，对整段文本排序；
2. `ObDomainDMLIterator::get_next_domain_rows()` 逐批取输入行；
3. `ObFTDMLIterator::generate_ft_word_rows()`（`ob_das_domain_utils.cpp:1135`）
   把一行文档展开成多行 word；
4. `ObPxMultiPartSSTableInsertOp` 期望有序输入写入 SSTable。

排序在第 1 步，展开在第 3 步，所以排序成本落在整段文本上；去掉/替换第 1 步
会破坏第 4 步的有序输入契约。

## 3. 方案

### E1-B（推荐）：展开后排序

- 在 `ObFTDMLIterator` 增加“收集-排序-输出”模式：
  1. 持续调用 `generate_ft_word_rows`，把 word 行放入全局缓冲；
  2. 输入结束后按 `(word, doc_id)` 排序；
  3. 按有序批次输出给插入算子。
- 缓冲使用 `ObChunkDatumStore`/`ObSortVec`，超限可落盘；
- DDL SQL 在该模式下不再生成整段 `ORDER BY`；
- 由 flag 门控，默认关闭。

### E1-A（备选）：scan 阶段展开

- 让 `ObTableScanOp` 直接输出多行 token，使 SQL sort 排在 token 行上；
- 更接近优化器语义，但改动 scan 契约，风险更高。

## 4. 里程碑

### E1.1：可行性原型（1 周）

- 在 `ObFTDMLIterator` 中离线收集一批 word 行并排序输出；
- 验证行内容/词频与 M0 一致；
- 不改变正式路径（flag 默认关闭）。

### E1.2：接入插入计划（2 周）

- 该模式下 DDL 不生成整段 ORDER BY；
- 插入算子消费有序 word 行；
- 构建成功、posting 行数 813,664、词频一致。

### E1.3：查询回归

- 官方命中 8001/11000/7332/20；
- 193 项结果对照一致。

### E1.4：性能与收口

- 同机 6 轮 paired A/B；
- 构建类 20%～30%；
- 内存/落盘上限、失败回退、文档。

## 5. 风险

- 813k 行全局排序的内存/落盘成本可能抵消排序收益；
- 插入算子的 slice/tablet 顺序契约需要确认；
- 并行 build 下每个 iterator 的缓冲边界；
- 若 E1.1 排序+写入 > 6.3s，停止并回到 R0 结论。

## 6. 当前状态

- R0 成本验证完成（posting 排序+写入 2.616s，整段 sort 3.1～4.9s）；
- M1.3 空表 create/drop、M2/M3 外部原型完成；
- 本文件是 E1 设计，尚未写执行器代码。

## 7. E3：TOKENIZE_TABLE 表函数方案

现有 SQL 层没有通用 table function 框架，唯一类似的是 JSON_TABLE 的专用算子。
E3 需要新增一个 token 展开算子：

### 7.1 Parser / Resolver

- 新增 `TOKENIZE_TABLE(expr, 'parser')` 表引用语法，解析为新的 table function 节点；
- resolver 生成逻辑算子 `LOG_TOKENIZE_TABLE`，输出列：
  `word, doc_id, word_count, doc_length`；
- 与 JSON_TABLE 一样作为 join 的右表/表函数处理。

### 7.2 Executor

- 新增 `ObTokenizeTableOp`：对输入行调用 `ObFTParseHelper`，一次分词生成多行；
- 输出 token 行，插入 child 直接消费；
- doc_id 从输入行 rowkey 生成。

### 7.3 DDL build

```sql
INSERT INTO aux (word_segment, doc_id, word_count, doc_length)
SELECT word, doc_id, word_count, doc_length
FROM docs, TOKENIZE_TABLE(word_segment, 'ik')
ORDER BY word, doc_id;
```

- sort 作用在 token 行上，去掉整段 word_segment 预排序；
- DML row cache 不再需要二次展开。

### 7.4 里程碑

- E3.1 parser/resolver 骨架；
- E3.2 executor 单机分词输出；
- E3.3 接入 DDL build，验证行数/命中；
- E3.4 paired A/B，目标 20%~30%。

### 7.5 风险

- parser 生成文件/语法兼容；
- 表函数与 join/下推/并行执行；
- doc_id 与现有 aux 生成规则一致；
- 工作量为多文件引擎特性，非本会话可完成。
