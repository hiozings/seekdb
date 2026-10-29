# 回退 BENG ASCII 快速路径的三版本对照（2026-10-29）

## 改动

`feature/fts-performance` 在 150a65103 上回退 `src/storage/fts/ob_beng_ft_parser.{h,cpp}`
的 ASCII 快速路径，恢复 `english_analyzer_` 原始路径；同时：

- 保留 150a65103 的 AI 空闲 worker 优化（无 AI 业务时不执行任务 SQL）。
- 保留 77fbb6050 的 IK 分类与 Tokenizer 热路径改动。
- 同步回退只针对 BENG 快速路径新增的单元测试。

## 对照版本

| 标签 | 提交 | 二进制 SHA-256 |
| --- | --- | --- |
| baseline | `5bce9e06d`（官方初始） | `d1ed8d37…c8ece3` |
| previous | `150a65103`（线上 0 分版本，保留 BENG 快速路径） | `59b947a8…20f87442` |
| candidate | 150a + 回退 BENG 快速路径 | `d8ffb99e…45a485c85` |

说明：本次 “previous” 取 150a65103，即线上最新提交，而不是更早的 77fbb6050；
后者二进制在环境清理后不可用，且本次要回答的是“回退 150a 的 BENG 快速路径是否
改善”，因此以 150a 为 before 更直接。官方 5bce9e0 仍作为独立基线。

## 方法

使用未修改的官方基准 `tools/benchmark/fts_large_bench.sh`，通过
`unittest/storage/fts/run_fts_comparison.py` 执行九轮均衡交错（每版本三轮）：

- 每轮独立实例目录、重新导入 20000 行、BATCH 500、ROUNDS 3000、
  QUERY_ROUNDS 200、SAMPLES 3、WARMUP 30、SKIP_LOAD 0。
- 服务端 `--parameter memory_limit=2G`（2g 组）或 `1G`（1g 组）、
  `cpu_count=2`，服务端固定 CPU 8,10，客户端固定 CPU 12，端口 12886。
- 全部原始报告见 `2g/`、`1g/`；`vs-official.json` 对 5bce9e0 计算，
  `vs-150a.json` 对 150a65103 计算。负提升全部保留，不做截断。

## 结果

### 2G

| 指标 | baseline | previous (150a) | candidate (回退) |
| --- | ---: | ---: | ---: |
| build_beng_en_sec | 1.8493 | 1.9257 | 1.8470 |
| build_ik_all_sec | 4.9300 | 4.5613 | 4.8293 |
| build_ik_content_sec | 3.3770 | 3.5230 | 3.1880 |

- candidate 相对 official：构建 **+2.59%**，分词 −4.67%，查询 −2.48%，
  三类等权 **−1.52%**。
- candidate 相对 150a：BENG 构建 **+4.09%**，构建类 +2.57%，分词 −4.00%，
  查询 −9.02%，三类等权 **−3.48%**。

### 1G

| 指标 | baseline | previous (150a) | candidate (回退) |
| --- | ---: | ---: | ---: |
| build_beng_en_sec | 1.8160 | 1.7463 | 1.8470 |
| build_ik_all_sec | 5.0980 | 4.4983 | 4.6950 |
| build_ik_content_sec | 3.1877 | 3.4657 | 3.2933 |

- candidate 相对 official：构建 **+0.96%**，分词 −0.04%，查询 −10.87%，
  三类等权 **−3.32%**。
- candidate 相对 150a：BENG 构建 **−5.76%**，构建类 −1.72%，分词 +1.54%，
  查询 −6.21%，三类等权 **−2.13%**。

## 解释与限制

- **BENG 构建没有稳定方向**：2G 下回退版比 150a 快约 4%，1G 下反而慢约 6%，
  两次对照方向相反；与官方基线的差异也都在 ±2% 内。本地三轮/版本不足以
  分辨这一量级的差异。
- **查询/分词波动很大**：1G 组 candidate 的 `query_cn`/`query_beng` 相对
  official 分别低约 24%/25%，但 candidate 与 150a 仅差 BENG 解析器，IK 查询
  路径未变，因此这些差异主要是宿主负载/频率/缓存噪声，不能归因于本次回退。
- **未复现线上 BENG 25% 退化**：本地 2G/1G 的 BENG 构建都在 1.75–1.93s 区间，
  与线上 5.328→6.675s 的差距不具可比性；线上分数仍需平台侧复测。
- 仓库自带固定基线对本次报告仍会给出满分（基线值远慢于本机），不能作为平台
  成绩依据；此前 `2026-10-29-fts-score-consistency` 审计已证明平台三次报告的
  均值无法由同一固定基线解释。
- **正确性**：回退后重建 `test_fts_hotpath`，5/5 用例通过；官方测试、基准、
  评分和工作流文件未修改。

## 复现

```sh
python3 unittest/storage/fts/run_fts_comparison.py \
  --baseline /path/to/5bce-seekdb \
  --previous /path/to/150a-seekdb \
  --candidate /path/to/reverted-seekdb \
  --output /tmp/fts-revert-2g --port 12886 \
  --server-cpus 8,10 --client-cpu 12 --memory-limit 2G

python3 unittest/storage/fts/summarize_fts_reports.py /tmp/fts-revert-2g \
  --output /tmp/vs-official.json
python3 unittest/storage/fts/summarize_fts_reports.py /tmp/fts-revert-2g \
  --baseline-label previous --output /tmp/vs-150a.json
```

1G 组将 `--memory-limit` 改为 `1G`、输出目录改为独立路径；不同资源组不得混算。
