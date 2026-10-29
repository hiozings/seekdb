# 全文索引热路径回归与性能对照

本目录是额外本地测试。官方 `tools/benchmark/fts_large_bench*` 文件及工作流保持原样。

## 原生正确性与微基准

在已经配置好的 CMake release 构建中启用 `SEEKDB_BUILD_COMPETITION_TESTS=ON`，执行：

```sh
cmake --build build_release --target test_fts_hotpath --parallel 2
build_release/unittest/test_fts_hotpath
FTS_MICROBENCH=1 taskset -c 8 build_release/unittest/test_fts_hotpath
ctest --test-dir build_release -L competition --output-on-failure
```

覆盖 UTF-8 全码点分类与旧实现的分类优先级、非法编码返回值、词流复用、大小写/标点/NUL，以及全部 65,536 个字节对的扫描结果。当前构建不提供 UTF16/UTF16LE，测试保留其拒绝行为；若构建启用 UTF16，则测试其代码单元。畸形字节测试有显式可读填充，逻辑输入长度仍为四个字节，避免旧长度解码器越过截断字符时导致测试自身越界。微基准不作为官方分数或硬性时间断言。

## 完整 SQL 输出

在专用实例运行官方完整基准、生成 `fts_large_bench.docs` 后：

```sh
python3 unittest/storage/fts/compare_fts_results.py --port 12886 --snapshot /tmp/before.json
# 保留相同数据库和索引，切换待验证二进制。
python3 unittest/storage/fts/compare_fts_results.py --port 12886 --snapshot /tmp/after.json --compare /tmp/before.json
```

比较完整 token 数组、词频、doc_len、smart/max_word、词长参数、全量匹配 ID、相关性分值和排序，并显式断言新增、更新、删除及回滚后的检索结果。测试只重建自身 `fts_hotpath_regression` 数据库，不能对生产实例执行。

不同索引重建的平均文档长度估计可能使相关性分值略有变化。因此严格数值对照必须使用同一份索引；跨独立索引构建另核对完整 token、匹配集和排名 ID，不能把统计分值变化误报为分词变化，也不能只比较命中计数。

## 六轮完整性能测量

准备两份固定的 release 二进制，在无业务负载的专用环境执行：

```sh
python3 unittest/storage/fts/run_fts_comparison.py \
  --baseline /absolute/before/seekdb --candidate /absolute/after/seekdb \
  --output /tmp/fts-comparison-new --port 12886 \
  --server-cpus 8,10 --client-cpu 12
python3 unittest/storage/fts/summarize_fts_reports.py /tmp/fts-comparison-new \
  --output /tmp/fts-summary.json
python3 -m unittest discover -s unittest/storage/fts -p test_report_summary.py
```

输出目录必须不存在，端口必须空闲。按本机实际 CPU 拓扑选择亲和性，两版本保持相同。运行器只停止自身 Popen 创建的数据库进程，不连接已有实例。顺序为基线、候选、候选、基线、基线、候选；每轮重启，内存2G/cpu_count2，完整加载20000行，BATCH500、ROUNDS3000、QUERY_ROUNDS200、SAMPLES3、WARMUP30、SKIP_LOAD0。官方脚本和其计时逻辑不变。

每轮独立报告、日志和二进制 SHA-256 保存在输出目录，所有样本进入均值。汇总先算类别内各项提升，再对三类别等权平均，保留负提升；缺指标或参数不一致会拒绝。`ci_reference_only` 使用原始固定基线，仅供异地环境参考，不代表平台得分。本地前后改善单列为 `local_mean_improvement`。多个目录可一起汇总，不挑选最优轮次。

## 官方初始基线和线上旧提交的三版本对照

`run_fts_comparison.py` 现在为每轮创建独立数据目录，避免不同提交的系统Schema或历史状态干扰结果。指定 `--previous /path/to/old-submission/seekdb` 后执行九轮均衡交错，每个版本三轮；`--memory-limit 1G` 可单独进行资源压力组。资源配置不同的报告不得混算。

```sh
python3 unittest/storage/fts/run_fts_comparison.py \
  --baseline /path/to/5bce9e0/seekdb --previous /path/to/77fbb605/seekdb \
  --candidate /path/to/fixed/seekdb --output /tmp/fts-three-way-new
python3 unittest/storage/fts/summarize_fts_reports.py /tmp/fts-three-way-new \
  --output /tmp/vs-official.json
python3 unittest/storage/fts/summarize_fts_reports.py /tmp/fts-three-way-new \
  --baseline-label previous --output /tmp/vs-submission.json
```

BENG新增回归覆盖稳定词串生命周期、600词文档的调用方分配次数上限、分配失败后重试、两种UTF8排序规则的全部ASCII字节对、非ASCII回退及64KiB边界。可选微基准在同一二进制中对比保留的通用分析器和有界ASCII路径，仅作为分配/归一化阶段证据，不替代完整建索引测量。
