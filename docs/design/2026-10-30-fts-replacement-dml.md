# FTS 替换式 DML 子工程（R0）

## 1. 目标

用 `(word, doc_id, word_count, doc_length)` posting 行直接替代当前
`word_segment` 整段排序的 build 路径：

- 共享现有 tokenize 结果，不重复分词；
- 按 `(word, doc_id)` 短键排序，去掉整段文本排序；
- 构建目标从现有 FTS aux 表切换为 posting aux 表；
- 查询 `MATCH` 走 posting 表，保持官方命中/结果；
- 评估构建类 20%～30% 目标。

## 2. 现状与成本

| 项 | 值 |
|---|---:|
| 正常构建 | 9.376s |
| docs 预排序后构建（无整段 sort） | 6.289s |
| 整段 sort 成本 | 约 3.1s（33%） |
| 引擎内写 813,664 posting 行 | 1.872s |
| 外部 ETL（tokenize + 写） | 12.185s（不可行） |

结论：机会在“去掉整段 sort + 直接写 posting”；外部 ETL/构建后再写表都不行。

## 3. 设计

### 3.1 Schema

- 复用 M1.3 的 posting aux 表：
  `word VARCHAR(1024) general_ci`、`doc_id VARCHAR(64) binary`、
  `word_count INT`、`doc_length INT`，rowkey `(word, doc_id)`；
- index type：`INDEX_TYPE_FTS_POSTING_LOCAL`。

### 3.2 Build 路径

1. `ObFtsIndexBuildTask` 创建 posting aux 表；
2. DDL SQL 不再按整段 `word_segment` 排序，目标表为 posting aux；
3. `ObFTIndexRowCache` / `ObFTDMLIterator` 在现有分词结果上直接生成
   `(word, doc_id, word_count, doc_length)` 行；
4. 插入计划的 sort key 使用 posting rowkey `(word, doc_id)`；
5. 现有 FTS aux 表可保留为兼容路径，由 flag 切换。

### 3.3 Query

- `MATCH` 查询分词后按 word 扫描 posting 表；
- 结果并集/相关性保持官方行为；
- M3 外部原型已验证并集命中 8001/7332/20。

## 4. 里程碑

### R0：成本与可行性验证（1 周）

- 用预生成 wordstage（813k 行）测量“按 (word, doc_id) 排序 + 写 posting”；
- 目标：< 6.3s（相对 9.376s 至少 33% 排序收益被利用）；
- 若 posting 写入+排序 > 6.3s，则 M4 不可达，停止。

### R1：posting aux schema/DML 原型（2～3 周）

- posting aux 进入 FTS build task 的 aux 列表；
- row cache 生成 posting 行；
- 构建后 posting 行数/词频与 M0 一致；
- 构建 A/B 记录。

### R2：查询集成（2～3 周）

- `MATCH` 走 posting 扫描；
- 4 条官方查询命中 8001/11000/7332/20；
- 193 项结果对照一致。

### R3：性能与收口

- 同机 6 轮 paired A/B；
- 构建类 20%～30%；
- flag 回退、文档、限制。

## 5. 风险

- posting 行需要在 DML 层生成，涉及插入计划/row descriptor；
- 现有 FTS aux 与 posting aux 的 sort key、SSTable 写入路径不同；
- 查询相关性/多值索引语义需要保持一致；
- 若 R0 成本不达标，应停止而不是继续实现。

## 6. 当前状态

- M1.3 空表 create/drop 已完成；
- M2/M3 外部原型已完成；
- 本文件是替换式 DML 的 R0 设计，尚未写 R0 测量脚本/代码。

## 7. R0 结果（2026-10-30）

用已填充的 813,664 行 posting 数据构造按 doc_id 排序的 wordstage，再执行：

```sql
INSERT INTO __fts_posting_r0
SELECT * FROM __fts_wordstage ORDER BY word, doc_id;
```

实测：

- `r0_sort_write_ms = 2.616s`；
- 写入行数 `813,664`；
- 对比：整段 `word_segment` sort 约 3.1s，正常构建 9.376s。

结论：按 `(word, doc_id)` 排序 + 写 81 万 posting 行只要 **2.6s**，低于整段
sort 的 3.1s，也远低于 6.3s 的 R0 门禁。只要共享现有 tokenize 结果并替换
aux 写入，替换式 DML 在成本上**可行**，20%～30% 目标有理论空间。

风险仍在 DML 集成：posting 行需要在插入计划/row descriptor 中生成，不能用
外部 ETL；R1 需要真正的 engine 侧原型。

### R0 第二轮 A/B（2026-10-30）

| 场景 | build 耗时 |
|---|---:|
| 正常 docs 顺序（第二轮） | 8.656s |
| docs 预排序后（第二轮） | 3.794s |

两轮 A/B：

| 轮次 | 正常 | 预排序 | 排序成本 |
|---|---:|---:|---:|
| 1 | 9.376s | 6.289s | 3.087s |
| 2 | 8.656s | 3.794s | 4.862s |

排序成本约 3.1～4.9s，而 posting 排序+写入实测 2.616s；替换式 DML 的
20%～30% 目标有成本空间，但主机噪声较大，R1 需要 paired 多轮验证。

## 8. CTE + JSON_TABLE ETL 结果（2026-10-30）

OceanBase 支持在 CTE 中把 tokenize 结果转成 JSON，再用 JSON_TABLE 展开：

```sql
INSERT INTO posting
SELECT jt.word, CAST(t.id AS CHAR), COUNT(*), JSON_LENGTH(t.tok)
FROM (
  SELECT id,
         CAST(tokenize(cast(concat(title, ' ', content) as char), 'ik') AS JSON) AS tok
  FROM docs
) t,
JSON_TABLE(t.tok, '$[*]' COLUMNS(word VARCHAR(1024) PATH '$')) jt
GROUP BY jt.word, t.id, JSON_LENGTH(t.tok);
```

实测 20000 文档：

- 耗时 **37.091s**；
- 写入 813,664 行，`SUM(word_count)=813,664`，内容正确；
- 对比正常构建 9.376s，仍慢约 4 倍。

结论：CTE 方案解决了 SQL 语法问题，但独立 ETL 的 tokenize + 展开 + GROUP BY
代价远超现有构建，不能作为 M4 路径；替换式 DML 必须共享现有 tokenize 结果。
