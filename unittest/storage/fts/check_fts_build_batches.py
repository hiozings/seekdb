#!/usr/bin/env python3
"""Offline FTS-build oracle for an owned disposable SQL instance."""
from compare_fts_results import run


def collect_build_batches(port):
    results = {}
    parameter = run(port, "SELECT value FROM oceanbase.__all_virtual_sys_parameter_stat "
                          "WHERE name='_rowsets_max_rows' LIMIT 1")
    assert parameter['returncode'] == 0, parameter
    original = int(parameter['output'].strip())
    database = 'fts_build_batch_regression'
    long_words = ' '.join(f'term{i:04d}' for i in range(700))
    texts = [None, '', 'alpha', long_words, 'alpha omega', None,
             '数据库 全文索引 分词器', ' ', 'tail']
    try:
        for batch in (0, 1, 7, 256):
            assert run(port, f'ALTER SYSTEM SET _rowsets_max_rows={batch}')['returncode'] == 0
            for partitioned in (False, True):
                suffix = ' PARTITION BY HASH(id) PARTITIONS 2' if partitioned else ''
                table = database + '.docs'
                setup = (f'DROP DATABASE IF EXISTS {database}; CREATE DATABASE {database};'
                         f'CREATE TABLE {table}(id INT PRIMARY KEY, title VARCHAR(100), body LONGTEXT){suffix};')
                values = []
                for i, text in enumerate(texts):
                    body = 'NULL' if text is None else "CONVERT(X'%s' USING utf8mb4)" % text.encode().hex()
                    values.append(f"({i+1},'title{i+1}',{body})")
                setup += f'INSERT INTO {table} VALUES ' + ','.join(values)
                value = run(port, setup)
                assert value['returncode'] == 0, value
                for parser in ('beng', 'ik'):
                    value = run(port, f'ALTER TABLE {table} ADD FULLTEXT INDEX ft_{parser}(title,body) WITH PARSER {parser}')
                    assert value['returncode'] == 0, (batch, partitioned, parser, value)
                    # Force the corresponding index, retaining the same result contract.
                    for query in ('alpha', 'term0000 term0256 term0512 term0699', '数据库 全文索引', 'notpresent'):
                        match = f"MATCH(title,body) AGAINST('{query}')"
                        sql = f'SELECT /*+ INDEX(docs ft_{parser}) */ id,{match} FROM {table} WHERE {match} ORDER BY id'
                        value = run(port, sql)
                        assert value['returncode'] == 0, (sql, value)
                        results[f'{batch}/{partitioned}/{parser}/{query}'] = value
                    plan = run(port, 'EXPLAIN ' + sql)
                    assert plan['returncode'] == 0 and f'ft_{parser}' in plan['output'], plan
                    if parser == 'beng':
                        # MATCH index selection does not reliably honor INDEX hints
                        # between two FTS indexes on the same columns. Leave exactly
                        # one parser index present for each oracle query.
                        dropped = run(port, f'ALTER TABLE {table} DROP INDEX ft_beng')
                        assert dropped['returncode'] == 0, dropped
                change = run(port, f"UPDATE {table} SET body='alpha' WHERE id=4; DELETE FROM {table} WHERE id=5")
                assert change['returncode'] == 0, change
                value = run(port, f"SELECT id FROM {table} WHERE MATCH(title,body) AGAINST('alpha') ORDER BY id")
                assert value['returncode'] == 0 and value['output'] == '3\n4\n', value
                results[f'{batch}/{partitioned}/dml'] = value
    finally:
        restored = run(port, f'ALTER SYSTEM SET _rowsets_max_rows={original}')
        assert restored['returncode'] == 0, restored
        assert run(port, f'DROP DATABASE IF EXISTS {database}')['returncode'] == 0
    return results
