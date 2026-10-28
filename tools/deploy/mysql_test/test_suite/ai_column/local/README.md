# 本地 AI COLUMN 回归

这些用例独立于官方 `t/`、`r/mysql/`，不覆盖官方预期结果。使用现有 mysqltest 和官方 `include/ai_column_mock.py`，将预期结果复制到临时目录后运行，勿使用 `--record`。

| 用例 | 覆盖 |
| --- | --- |
| `ai_column_transaction_edges` | 保存点、重复键忽略、REPLACE、语句失败回滚、无变化更新 |
| `ai_column_ddl_backfill` | 已有数据 ADD/MODIFY 补算、视图与隐式列清单写保护 |
| `ai_column_key_encoding` | 二进制与 UTF-8 主键、16 KiB 主键、出行 LOB 谓词 |
| `ai_column_publication_failure` | 唯一键发布失败、CHECK 联动列、无关任务继续处理 |
| `ai_column_timestamp_key` | 跨时区定位与清理、1900 年微秒值、零时间戳 |

从 `tools/deploy` 运行时，Mock 相对路径可直接解析。连接参数指向专用测试实例；各用例使用端口 19125–19128、19130，避免与官方 Mock 端口及其他测试冲突。

## Endpoint 并发配置

`CREATE_AI_MODEL_ENDPOINT` 和 `ALTER_AI_MODEL_ENDPOINT` 的 JSON 参数支持 `max_concurrency`：必须为 1–1024 的整数，创建时省略默认为 1，局部 ALTER 省略则保留原值。字符串、浮点数、布尔值、NULL 和越界值返回 11116。持久化值可从 `oceanbase.DBA_OB_AI_MODEL_ENDPOINTS.MAX_CONCURRENCY` 查询。

当前 AI COLUMN 后台共用一个执行槽，所有 Endpoint（包括指向相同 URL 的别名）合计最多一个模型调用。配置表示上限，不会按该数值扩容 worker；当前实际并发度保守地不超过任何合法配置值。同步 AI SQL 函数不属于这个后台执行槽。
