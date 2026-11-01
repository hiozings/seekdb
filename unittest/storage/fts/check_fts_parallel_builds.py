#!/usr/bin/env python3
"""Full-result oracle for FTS builds with parallel DDL and sampled range cuts."""
from compare_fts_results import run


def collect_parallel_builds(port):
    results = {}
    database = 'fts_parallel_build_regression'

    def checked(sql):
        result = run(port, sql)
        assert result['returncode'] == 0, (sql[:300], result)
        return result

    old_batch = int(checked("SELECT value FROM oceanbase.__all_virtual_sys_parameter_stat "
                            "WHERE name='_rowsets_max_rows' LIMIT 1")['output'].strip())
    modes = [('default', '', '', 256, False, 1),
             ('hint1', '', '/*+ parallel(1) */', 7, False, 1),
             ('hint6', '', '/*+ parallel(6) */', 256, False, 1),
             ('forced6', 'SET _force_parallel_ddl_dop=6;', '', 256, True, 1),
             ('disabled', 'SET _enable_parallel_ddl=false;', '', 256, True, 1),
             ('table4', '', '', 256, False, 4),
             ('index4', '', '', 256, False, 1)]
    try:
        for name, session, hint, batch, partitioned, table_dop in modes:
            checked(f'ALTER SYSTEM SET _rowsets_max_rows={batch}')
            suffix = ' PARTITION BY HASH(grp) PARTITIONS 2' if partitioned else ''
            table = database + '.docs'
            checked(f'DROP DATABASE IF EXISTS {database}; CREATE DATABASE {database};'
                    f'CREATE TABLE {table}(grp INT,id INT,title VARCHAR(100),body TEXT,'
                    f'PRIMARY KEY(grp,id)) PARALLEL {table_dop}{suffix}')
            # Insertion order differs from composite primary-key order, so a
            # sampled source row cannot borrow an unrelated domain-table row.
            for start in range(0, 4096, 256):
                rows = []
                for sequence in range(start, start + 256):
                    i = sequence * 73 % 4096
                    text = ('alpha' if i % 3 == 0 else 'beta') + ' 数据库 全文索引 ' + f'token{i:04d}'
                    if i % 97 == 0:
                        body = 'NULL'
                    elif i % 89 == 0:
                        body = "''"
                    else:
                        body = "CONVERT(X'%s' USING utf8mb4)" % text.encode().hex()
                    rows.append(f"({i % 7},{i},'title marker{i % 17}',{body})")
                checked(f'INSERT INTO {table} VALUES ' + ','.join(rows))
            for parser in ('beng', 'ik'):
                index_dop = ' PARALLEL 4' if name == 'index4' else ''
                checked(session + f'CREATE {hint} FULLTEXT INDEX ft ON {table}(title,body) WITH PARSER {parser}{index_dop}')
                checked(f'ANALYZE TABLE {table}')
                for query in ('alpha', '数据库', 'token0000 token1024 token4095', 'marker3', 'notpresent'):
                    match = f"MATCH(title,body) AGAINST('{query}')"
                    sql = f'SELECT grp,id,{match} FROM {table} WHERE {match} ORDER BY grp,id'
                    results[f'{name}/{parser}/{query}'] = checked(sql)
                checked(f"UPDATE {table} SET body='alpha 数据库' WHERE MOD(id,101)=0;"
                        f'DELETE FROM {table} WHERE MOD(id,113)=0')
                results[f'{name}/{parser}/dml'] = checked(
                    f"SELECT grp,id FROM {table} WHERE MATCH(title,body) AGAINST('alpha') ORDER BY grp,id")
                checked(f'ALTER TABLE {table} DROP INDEX ft')
        checked(f'DROP DATABASE IF EXISTS {database}; CREATE DATABASE {database};'
                f'CREATE TABLE {database}.empty_docs(id INT PRIMARY KEY,body TEXT)')
        for parser in ('ik', 'beng'):
            checked(f'CREATE FULLTEXT INDEX ft ON {database}.empty_docs(body) WITH PARSER {parser}')
            results[f'empty/{parser}'] = checked(
                f"SELECT id FROM {database}.empty_docs WHERE MATCH(body) AGAINST('alpha') ORDER BY id")
            checked(f"INSERT INTO {database}.empty_docs VALUES(1,NULL),(2,''),(3,'alpha')")
            results[f'empty/{parser}/dml'] = checked(
                f"SELECT id FROM {database}.empty_docs WHERE MATCH(body) AGAINST('alpha') ORDER BY id")
            checked(f'DELETE FROM {database}.empty_docs; ALTER TABLE {database}.empty_docs DROP INDEX ft')
    finally:
        checked(f'ALTER SYSTEM SET _rowsets_max_rows={old_batch}')
        checked(f'DROP DATABASE IF EXISTS {database}')
    return results
