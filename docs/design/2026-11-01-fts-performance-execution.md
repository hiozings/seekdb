# FTS 满分目标实施与证据记录

## 目标与授权

用户要求保持 TOKENIZE/MATCH 语义、官方数据/SQL/计时/评分文件不变，三类等权耗时下降至少 50%。2026-11-01 用户授权自主选择并持续实现；提交、合并、推送仍需另行同意。

正式满分必须由统一评测环境确认，本地同机 A/B 是优化证据，套用异地固定基线不是满分证明。

## 方案

优先保留现有索引格式，优化分词初始化/临时容器、构建重复工作和纯布尔 MATCH 查询。备选 compact posting 目前同时构建旧 aux，DML 一致性尚未闭环，保持默认关闭。单纯提高 DDL 并行度已有负结果，不作为主要路线。

参考公开实现：oceanbase/seekdb PR #2247、#2057、#2268、#2287。逐项验证后采用思想或适配代码，记录来源；不把公开 PR 的性能声明视作已验证证据。尤其不采用将 general_ci 等价关系替换为二进制比较的捷径。

## 约束与门禁

- 复用 feature/fts-structural-rewrite 当前工作区，保留用户未提交的 compact 实验。
- 默认开关下验证全部路径；不得靠仅在本地打开的开关声称提交有效。
- 分词需对照所有五种 parser、默认/显式配置、混合字符集、空串/NULL、词频/文档长度。
- 查询需验证完整 ID、分数、排名、布尔组合、附加过滤、LIMIT/OFFSET、DML/事务/重启。
- 构建需保持 aux 行内容与排序语义，必要时覆盖并发和异常清理。
- 新 C++ 类型不加 Ob 前缀；新头用 pragma once；自建测试在 unittest。

## 测量

专用容器 seekdb-official-tests-20261028 挂载当前仓库 /work，启动命令仅 sleep infinity，不启动已有数据库。沿用隔离 A/B 工具，2 CPU / 2G，server CPU 8,10，client CPU 12，每轮独立实例，完整官方参数，六轮交错。

初始证据：build_bench/20261101-start-ab。基线二进制 official-5bce-seekdb，候选为本轮修改前 build_release/src/observer/seekdb；运行器保存 SHA256。

## 进展

- 已阅读当前工作区、历史实验和公开 PR；初始六轮 A/B 已启动。
- 后续结果在本文件追加；没有实际测试支持的收益不得标为完成。

## 已验证候选与后续授权

已实现默认分词属性复用、有界TOKENIZE结果缓存与IK分词缓存、局部容器去锁、严格门控的免评分查询、批量归并/投影、COUNT-only LIMIT视图裁剪、差分字符串批量解码、固定二进制ID编码选择。参考代码来自公开PR [#2268](https://github.com/oceanbase/seekdb/pull/2268) 和 [#1787](https://github.com/oceanbase/seekdb/pull/1787)，增加了缓存代际、完整键与词序、保守SQL门控及边界回归。

原生10个目标、514项同索引完整输出、508项新建索引对照、原样TOKENIZE与9组官方比赛SQL通过。独立审查未确认Major/Blocker。官方测试资产未改。

五组独立六轮A/B的综合下降分别为32.180%、39.537%、47.025%、36.281%、41.916%；最后一组经用户授权临时将CPU8/10设为performance，两个版本同配置，结束已恢复powersave。不能混算不同配置或只选最好样本，尚未证明达到50%目标。完整证据在docs/validation/2026-11-01-fts-*。

用户已授权提交并推送已验证工作区至origin/feature/fts-performance-20261101，保留默认关闭compact实验。正式评测入口/报告仍待提供，不能宣称正式满分。
