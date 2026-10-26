# File SQL：本地文件查询与验证

本功能直接接入 seekdb SQL 解析、动态列绑定、优化器及原生执行器；Rust `file-sql` 仅处理文件格式和受限文件 I/O，不执行 SQL。Apache parquet crate 用于 Parquet 解码；PyArrow 仅用于测试数据生成。

## 接口

默认 `enable_file_sql=OFF`。使用者必须具有 FILE 权限；管理员通过本地 socket 将 `secure_file_priv` 设置为专用测试目录。空值和 `NULL` 都禁止 File SQL；这里没有使用旧接口的空目录放行规则。

```sql
SET enable_file_sql=ON;
SELECT id,name FROM read_csv('/approved/data.csv',header=>true,sample_size=-1);
SELECT * FROM read_json('/approved/data.jsonl'); -- read_ndjson 同义
SELECT id FROM read_parquet('/approved/data.parquet');
SELECT file FROM glob('/approved/*.csv');
DESCRIBE SELECT * FROM read_csv('/approved/data.csv');
SELECT a.id,b.label FROM read_csv('/approved/a.csv') a
LEFT JOIN read_json('/approved/b.jsonl') b USING(id);
COPY (SELECT id,name FROM read_csv('/approved/data.csv') ORDER BY id)
TO '/approved/output.csv' (FORMAT CSV,HEADER true,DELIMITER '|',NULL 'nil');
COPY test.table_name TO '/approved/table.csv';
```

路径是 SQL 字符串常量。允许显式别名和 `name=value` / `name=>value` 命名参数；不支持参数化路径、多文件合并、远程 URL、嵌套文件类型和分布式扫描。glob 只匹配最后一个路径分量，返回排序后的单列 `file`。

CSV 默认 `header=true, delimiter=',', quote='"', escape='"', nullstr=''`；`delim`/`null` 是选项别名。`sample_size` 只能为 `-1`。分隔符、引号和转义符必须是单字节；支持 CRLF/LF、UTF-8 BOM、双引号转义、自定义转义和引号中的换行。未加引号的 NULL 文本表示 NULL；带引号的相同文本表示字符串。COPY 的 NULL 文本不能包含分隔符、引号或换行；否则在创建输出前失败。

CSV 和 JSONL 都完整流式遍历输入推断 Schema，然后重新打开只读扫描。JSONL 仅接受扁平对象，按首次出现顺序合并字段，缺失字段补 NULL，拒绝重复键、数组和嵌套对象。JSON 字符串保留 VARCHAR 类型。

类型包括 BIGINT、DOUBLE、BOOLEAN、VARCHAR、DATE、DATETIME、NULL。整数超出 int64 时保留为文本；整数与浮点混合时，仅当整数可由 double 精确表示才宽化为 DOUBLE，否则为 VARCHAR。日期由日历规则校验；冲突宽化为文本；全 NULL 列保持 NULL。CSV 无类型元数据，COPY 不保证数字字符串读回后仍为 VARCHAR。

Parquet 支持平面 BOOLEAN、INT32/INT64、FLOAT/DOUBLE、UTF8、DATE 和无时区毫秒/微秒 TIMESTAMP；无符号 int64 以文本保全。支持 nullable、多 Row Group、PLAIN/字典、Data Page V1/V2、无压缩及 Snappy。其他压缩或复杂类型受控报错；未投影列不读取数据页。

## 安全和资源边界

逐路径分量使用目录句柄及 no-follow 检查；拒绝 `..`、符号链接、非普通文件和授权目录外路径。推断、扫描批次和重扫核对 inode、设备、长度和纳秒 mtime/ctime；检测到文件变化即报错。文件查询禁用计划缓存插入并使用串行原生计划。

CSV/JSONL 单记录上限 16 MiB，列数上限 4096，名称上限 1024 字节，批次细胞预算 64 MiB；glob 最多 10000 个结果。Parquet Footer 上限 16 MiB，Compact 集合、层级、Schema 子节点和页尺寸在解码前校验，投影列页工作集预算 64 MiB。Footer 和页头校验后使用相同字节快照，避免重读竞态；元数据负长度/偏移在进入库的断言前拒绝。推断检查查询截止时间，执行检查取消状态。原生排序和 JOIN 的内存另受引擎自身管理。

COPY 将查询结果写入同目录独占临时文件，成功后原子 no-replace 发布。目标已存在、表达式失败、UTF-8 无效、授权变更和目录变化均报错并清理临时文件。输入始终只读。

## 运行测试

在可构建 seekdb 的 Linux 环境：

```bash
./build.py build -j4 seekdb
./build.py test -j4 //unittest/sql/parser:test_file_sql_parser \
  //unittest/sql/engine/basic:test_file_scan_utils --test_output=errors
(cd rust && cargo test -p file-sql)
(cd rust && cargo clippy -p file-sql --all-targets -- -D warnings)
```

运行专用实例（默认端口 12883，socket `/tmp/seekdb-file-sql-instance/run/sql.sock`），安装测试夹具依赖 `pyarrow==20.0.0`。以下脚本只更改专用实例的 `secure_file_priv`，结束后恢复；不创建管理员账号或授予权限。不要在共享生产实例执行。

```bash
python3 tools/deploy/mysql_test/test_suite/file_sql/run_file_sql_tests.py
python3 tools/deploy/mysql_test/test_suite/file_sql/run_mysqltest.py
FILE_SQL_RESOURCE_TEST=1 python3 tools/deploy/mysql_test/test_suite/file_sql/run_file_sql_tests.py \
  FileSQL.test_streaming_memory_and_descriptors
```

通过 `FILE_SQL_TEST_PORT` 和 `FILE_SQL_TEST_SOCKET` 指定实例。`run_mysqltest.py` 使用仓库 mysqltest，五组测试的部署和清理由 include 脚本完成；`--record` 仅用于有意更新并人工核对预期结果。资源测试生成约 206 MB CSV，采样服务器 RSS，并循环成功/失败查询及超时查询检查 FD 数量。

本地测试覆盖八项功能及 COPY；不代表组委会隐藏测试或正式评分结果。完整仓库 Sanity、内存插桩和全部普通 SQL 回归应由具备相应环境的 CI 继续执行。

## 本次本地验证记录（2026-10-26）

- seekdb 原生构建成功；两个 C++ 测试目标、四个用例通过。
- Rust 10 个用例通过，Clippy `-D warnings` 通过；含畸形 Parquet Footer/类型标签/负偏移、页头并发变化、CSV BOM 和 COPY NULL 标记边界。
- SQL 集成 16 项通过，包含 COPY 表/查询导出、浮点等值读回、授权变化清理、USING JOIN、流式资源验证。混合压缩夹具只有 id 列使用 Snappy，其他列使用 Gzip；只查 id 成功、查 name 受控报错，证明列裁剪作用于实际数据读取。
- 五组原生 mysqltest 结果已核对并回放；现有 `expr/func_length.test` 回归通过。
- 206,288,901 字节 CSV 扫描：两次采样 RSS 增量分别为 256 KiB、0 KiB，FD 数均保持 18→18。该结果是本次运行观测，不代表独立内存泄漏检测。
- 独立代码审查发现的问题均已修复并补回归。
- Sanity 命令 `./build.py test -j4 --sanity //unittest/sql/engine/basic:test_file_scan_utils --test_output=errors` 在分析阶段失败：`//bazel/third_party:sanity_pass` 无文件，环境缺少 `libsanitypass.so`。未完成 Sanity/ASan 检测及全仓库 SQL 回归。

## 2026-competition 合并约定

官方 `file_sql_basic.test`、对应预期结果和 `data/basic.csv`/`data/basic.jsonl` 保持比赛分支版本。此前自建的基础用例移至 `local/file_sql_basic_local.test`，自建夹具为 `local/basic.csv`/`local/basic.jsonl`；生成器在隔离目录中仍以 `basic.csv`/`basic.jsonl` 部署，避免两套数据互相覆盖。`run_mysqltest.py` 运行官方 `t/` 中除官方基础用例外的四组自建用例，以及 `local/` 下的自建基础用例；官方基础用例通过比赛部署脚本运行，依赖 `OBSERVER_DIR` 等环境。AI 同名自建用例保留在 `ai_function/local/` 下。历史验证记录仅对应自建用例；官方新增测试需要单独验证。
