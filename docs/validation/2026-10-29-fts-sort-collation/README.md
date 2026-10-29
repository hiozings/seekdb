# FTS 建索引排序的 general_ci 比较优化（2026-10-29）

## 采样方法

在带 `SYS_PTRACE` 的临时容器中启动 release 版 `seekdb`，灌入官方基准的
20000 行数据，执行：

```sql
ALTER TABLE docs ADD FULLTEXT INDEX fti_all_ik(title, content) WITH PARSER ik;
```

用 `eu-stack -p <tid> -1` 对 PX_G0 工作线程按 ~10ms 间隔采样。`perf` 不可用，
普通容器也禁止 ptrace，因此使用该独立容器；采样只读栈，不修改实例。

## 采样结论

构建 CPU 约 5.2s / 3.5s wall（2 核），热点依次为：

1. `ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row`
   → `ObSortOpImpl::sort_inmem_data` → `std::__introsort_loop`
   → `ObSortOpImpl::Compare::operator()`
   → `ObNullSafeDatumStrCmp<CS_TYPE_UTF8MB4_GENERAL_CI, false, true>::cmp`
   → `ObCharset::strcmpsp`。
   每个比较都走完整排序规则处理器，是排序的主要 CPU 开销。
2. `ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row`
   → `ObMacroBlockWriter::append_row`（逐行写 SSTable），次要热点。
3. 4 个 PX_G0 线程跑在 2 个 CPU 上，构建是 CPU 受限而非 IO 受限。

`EXPLAIN`/诊断日志确认该 DDL 排序没有走
`ObOptimizerUtil::check_can_encode_sortkey` 的编码排序键路径，因此比较器
无法使用 memcmp 快速键。

## 本次改动

`src/oblib/lib/charset/ob_charset.cpp` 的 `ObCharset::strcmpsp` 增加两个
不改语义的快速路径：

- 两段字符串字节完全相同（长度相等且 `memcmp == 0`）时直接返回相等。
  全文排序中同一个词会出现在大量文档里，相同键比较非常频繁。
- `utf8mb4_general_ci`、`cmp_endspace == false`、且两侧都是 ASCII 字母/数字
  时，用大小写折叠的逐字节比较。非 ASCII（中文词）和非字母数字仍走原
  排序规则处理器。

## 正确性

- 官方基准查询命中数不变：`query_cn_hits=8001`、`query_beng_hits=11000`、
  `query_mixed_hits=7332`、`query_limit_hits=20`。
- `test_fts_hotpath` 5/5 通过。
- 未改动官方基准、评分、工作流文件。

## 性能（同一实例、同一数据、drop + add `fti_all_ik`，各 3 次）

| 版本 | 1 | 2 | 3 | 中位数 |
| --- | ---: | ---: | ---: | ---: |
| 改动前（d8ffb99） | 3710ms | 3458ms | 3654ms | 3654ms |
| 改动后 | 3414ms | 3262ms | 3253ms | 3262ms |

中位数 **-10.7%**。整轮基准单次对照（含 load/分词/查询）总构建
8.717s → 8.460s（-2.9%），`build_ik_content` 3.229s → 2.950s；宿主负载
波动较大，整轮数字只作参考。

## 未完成目标与下一步

本次没有达到 50% 的目标。剩余热点仍是通用的内存排序及其逐次排序规则
比较，单靠比较函数快速路径不足。下一步优先级：

1. 为 FTS 辅助索引构建的排序启用编码排序键（或等价的专用多值排序），
   把比较降为 memcmp；需要改 DDL 构建计划而不是优化器入口。
2. 将 `write_ordered_slice_by_row` 的逐行 SSTable 写入改为批量 append。
3. 评估 PX 工作线程数与 2 核的匹配（当前 4 线程争抢 2 核），减少上下文
   切换和缓存抖动。

所有改动需在相同实例、相同数据、多轮交替下复核；宿主噪声足以淹没
5% 量级的差异。
