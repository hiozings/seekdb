# MiniVectorDB

轻量单机向量数据库：基础 SQL、向量检索、持久化存储、事务与崩溃恢复。

> 当前状态：里程碑 **M3**（协议层 + SQL 解析 + 持久化页存储 + B+tree 主键索引 + WAL 事务与崩溃恢复）。
> 向量索引（M4）与 WAL 崩溃恢复（M3）尚未接入；README 会随里程碑更新。

## 1. 语言与构建

- 语言：C++20
- 构建：CMake ≥ 3.20 + 任意 C++20 编译器（本地验证：GCC 16.2.1；镜像内：Ubuntu 24.04 自带 GCC 13）
- 无第三方运行时依赖

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## 2. 容器

```bash
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest
```

镜像内入口为 `/usr/local/bin/minivectordb --data-dir /data`，前台运行，
**不监听任何端口**，只通过标准输入接收 SQL、通过标准输出返回结果。
日志与诊断一律写入标准错误，不污染标准输出。

使用 `-i`，不要使用 `-d` 或 `-t`：评测驱动通过进程管道通信。

## 3. 运行协议

- 语句以 `;` 结尾，可跨行；`'...'` 字符串与 `--`、`/* */` 注释内部的 `;` 不切分。
- 输出为制表符分隔的文本：先一行列名（`SELECT`），再逐行数据。
- 错误输出为 `ERROR <code>: <message>`，出错后会话继续。
- 结束方式：标准输入 EOF，或输入 `quit` / `exit` / `\q`。
- 诊断开关：`--probe` 把每条语句与执行结果打到 stderr；`--stats` 在退出时报告进程峰值内存。

### 3.1 线格式开关

题包协议发布后可零代码调整输出格式：

| 环境变量 | 取值 | 默认 |
| --- | --- | --- |
| `MVDB_PRINT_HEADER` | `1`/`0` | `1` |
| `MVDB_SEP` | 分隔符，支持 `\t`/`\n` | `\t` |
| `MVDB_FLOAT_FMT` | printf 格式 | `%.6f` |
| `MVDB_DML_OUTPUT` | `none`/`affected`/`ok` | `none` |
| `MVDB_ERROR_FMT` | 含 `{code}`、`{message}` 的模板 | `ERROR {code}: {message}` |
| `MVDB_READY` | 启动时打印的就绪标识 | 空（不打印） |
| `MVDB_OK_FMT` | `MVDB_DML_OUTPUT=ok` 时输出的文本 | `OK` |

## 4. 数据目录与持久化

所有数据都在 `--data-dir`（评测环境为 `/data`）：

```
/data/
  catalog.meta     表定义（原子写入：临时文件 + fsync + rename）
  t<id>.heap       每表一个堆文件，8 KiB 槽页
  t<id>.btree      主键索引（B+tree，page 0 为元数据页）
  wal.log          预写日志（整页镜像 + 提交记录，CRC32 校验）
  wal.log.epoch    日志纪元，保证页 LSN 跨重启单调
```

- 行按模式紧凑编码：`BIGINT` 8 字节、`VARCHAR` 长度+UTF-8 字节、`VECTOR` `dim×4` 字节。
- 页缓存在内存中，默认 256 页（2 MiB）；**数据量超过缓存预算时按页换入换出，不依赖全量载入内存**。
- 未显式 `BEGIN` 的写语句自动提交：提交时把本事务修改过的页以整页镜像写入 WAL，
  追加 COMMIT 记录并 `fdatasync`，之后才返回成功。
- 事务未提交的页**不允许被换出到数据文件**（页缓存拒绝逐出有属主的页），
  因此磁盘上只存在已提交的数据，恢复不需要 undo。
- 重启时重放已提交事务的页镜像；页头 LSN 保证重放幂等，恢复过程中被再次终止也能收敛。
- 崩溃模型限定为进程被强制终止（赛题定义），不模拟断电，因此页写入未做校验和。
- 进程正常退出或收到 `quit` 时，**未提交事务先回滚**再落盘。
- 重启后表定义、数据与主键唯一性均恢复；主键索引直接从 `.btree` 文件加载，
  **不扫描堆文件**，因此启动成本与行数无关。
- 索引构建被中断时（标志位未置位）下次启动会重建；`insert` 是 upsert，重建幂等。

## 5. 已支持的 SQL 子集

| 类别 | 语法 |
| --- | --- |
| 建表 | `CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(n), v VECTOR(d))` |
| 写入 | `INSERT INTO t [ (列名, ...) ] VALUES (...), (...)` |
| 查询 | `SELECT *` / 普通列 / `L2_DISTANCE(向量列, '[..]') [AS 别名]`，`WHERE`（`= < <= > >=` 与 `AND`），`ORDER BY`（含别名），`LIMIT` |
| 修改 | `UPDATE t SET 列 = 值, ... [WHERE ...]` |
| 删除 | `DELETE FROM t [WHERE ...]` |
| 事务 | `BEGIN` / `START TRANSACTION`、`COMMIT`、`ROLLBACK` |

明确的语义约束：

- 每表恰好一个 `BIGINT` 主键，主键不可重复、不可更新。
- 每表最多一个 `VECTOR` 列；向量为 `float32`，拒绝 NaN、Infinity、维度不符与 `float32` 溢出。
- `VARCHAR(n)` 的 `n` 按 Unicode 码点计数，字符串比较按码点顺序（UTF-8 字节序等价）。
- 距离按存储后的 `float32` 向量计算；`L2_DISTANCE` 升序，距离并列时按主键升序。
- 语句失败不留下任何部分修改；事务内的查询与向量检索都能看到本事务已完成的修改。
- 不支持：`NULL`、`JOIN`、聚合、子查询、`ALTER TABLE`、外键、嵌套事务、`OR`、`DROP` 等，均返回明确错误而不是静默执行。

## 6. 错误码

沿用 MySQL 约定，便于题包发布后按需重映射：

| 码 | 含义 |
| --- | --- |
| 1064 | 语法错误 |
| 1050 | 表已存在 |
| 1146 | 表不存在 |
| 1054 | 列不存在 |
| 1060 | 列名重复 |
| 1062 | 主键冲突 |
| 1068 | 多个主键 |
| 1173 | 缺少主键 |
| 1235 | 不支持的特性 |
| 1264 | 取值越界（VARCHAR 超长、float32 溢出） |
| 1366 | 取值类型错误（维度不符等） |
| 1105 | 内部错误 |

## 7. 从空数据目录开始的示例

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/minivectordb --data-dir /tmp/mvdb
```

```sql
CREATE TABLE docs (
    id BIGINT PRIMARY KEY,
    category VARCHAR(32),
    content VARCHAR(1024),
    embedding VECTOR(4)
);

INSERT INTO docs VALUES
    (1, 'database', '事务与恢复', '[1, 0, 0, 0]'),
    (2, 'database', '向量索引',   '[0, 1, 0, 0]'),
    (3, 'travel',   '旅行攻略',   '[0, 0, 1, 0]');

SELECT id, content,
       L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance
FROM docs
WHERE category = 'database'
ORDER BY distance
LIMIT 2;

BEGIN;
UPDATE docs SET embedding = '[0.8, 0.2, 0, 0]' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
ROLLBACK;

SELECT id FROM docs ORDER BY id;
```

输出：

```text
id	content	distance
1	事务与恢复	0.141421
2	向量索引	1.272792
id
1
2
3
```

退出后重新启动同一 `--data-dir`，`SELECT` 仍返回上面的三行——数据已持久化。

## 8. 本地验证

```bash
python3 tests/conformance/driver.py            # SQL 与重启一致性用例
python3 tests/conformance/driver.py -v         # 失败时显示差异
python3 tests/conformance/driver.py --update   # 重新生成期望输出
python3 tests/storage/large_dataset.py --rows 20000 --dim 128   # 超出页缓存的数据集
python3 tests/crash/kill_test.py --trials 8     # 强制终止后的恢复不变量
```

## 9. 测试与设计文档

- `docs/design.md`：存储布局、索引、事务、提交持久化与恢复流程
- `tests/conformance/`：SQL 级一致性用例（`cases/` 单进程、`restart/` 跨重启）
- `tests/storage/`：大数据集与页缓存压力测试
- `notes/evidence.md`：开发过程中的验证记录

## 10. 已知限制

- WAL 只在正常关闭或恢复完成后截断，长时间大批量写入期间日志会持续增长（M5 规划周期性 checkpoint）。
- 每次提交都写整页镜像；大事务的日志量约为脏页数的 8 KiB 倍，行级逻辑日志可进一步压缩（M5）。
- B+tree 删除只摘除键，不做节点合并；长期大量删除后索引文件不会收缩。
- 尚无向量索引，`ANN` 模式不存在，所有向量检索均为精确扫描（M4）。
- 排序与结果集在内存中完成；超大结果集的 `ORDER BY` 未做外排。
- 错误码为 MySQL 约定的推测值，待题包发布后按官方表重映射。
