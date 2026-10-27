# seekdb 赛题一部署成功记录

验证时间：2026-10-27T10:57:11+08:00（Asia/Shanghai）。

- 部署方式：Docker，已有容器 `seekdb-2026-practice`。
- 镜像名称：`oceanbase/seekdb:latest`；实际 SQL 返回版本见下方。
- 运行目录：`/var/lib/seekdb`。
- 宿主机连接地址：`127.0.0.1:2881`。
- 已执行 `docker start seekdb-2026-practice`，验证时容器为 running，完成验证后保持运行。
- SQL 验证通过 TCP 连接容器内的 `127.0.0.1:2881`，执行 `SELECT 1;`，返回 `1`。

## 遥测 ID

从实例 `/var/lib/seekdb/run/telemetry.json` 的 `content.id` 重新读取，与重启前完全一致：

```text
9cfa8c9c-f48b-8230-b92b-789f0a62a228
```

提交平台时只复制该 ID。此记录未代替大赛平台提交；尚未执行平台提交。

## SQL 原始输出

命令：

```bash
docker exec seekdb-2026-practice mysql --protocol=TCP -h127.0.0.1 -P2881 -uroot --batch -e 'SELECT 1; SELECT VERSION() AS server_version;'
```

```text
1
1
server_version
5.7.25-OceanBase seekdb-v1.4.0.0
```

## 部署状态原始输出

```text
/seekdb-2026-practice status=running started_at=2026-10-27T02:54:04.587714957Z ports={"2881/tcp":[{"HostIp":"127.0.0.1","HostPort":"2881"}]}
```

同目录保存 `sql-verification.txt`、`container-status.txt` 和 `telemetry-id.txt`。这些文件是实际命令输出或从实际遥测文件提取的数据，不是示例 ID。未制作截图。
