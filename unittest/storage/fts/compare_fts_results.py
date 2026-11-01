#!/usr/bin/env python3
"""Capture/check complete FTS outputs on a disposable benchmark instance.
Run after the unchanged official benchmark has populated fts_large_bench.docs.
The only database created/dropped by this extra regression is fts_hotpath_regression.
"""
import argparse
import json
from pathlib import Path
import random
import subprocess


def run(port, statement):
    result = subprocess.run(['mysql', '-h127.0.0.1', '-P' + str(port), '-uroot',
                             '-N', '-B', '--raw', '--default-character-set=utf8mb4'],
                            input='SET NAMES utf8mb4; ' + statement + ';\n',
                            text=True, capture_output=True, timeout=120)
    # Compare error codes, not contextual SQL snippets or runtime log timestamps.
    error = result.stderr.split(':', 1)[0] if result.returncode else ''
    return {'returncode': result.returncode, 'output': result.stdout, 'error': error}


def collect(port):
    results = {}
    # Repeated calls and row-wise input changes exercise worker-local memo hits,
    # misses, eviction, parser identity and the oversized-input fallback.
    for parser in ('ik', 'beng', 'space', 'ngram', 'ngram2'):
        for index, text in enumerate(('Alpha 中文 123', '', 'a\x00b', 'x' * 4097)):
            literal = "CONVERT(X'%s' USING utf8mb4)" % text.encode().hex()
            sql = f"SELECT TOKENIZE({literal},'{parser}'),TOKENIZE({literal},'{parser}')"
            results[f'memo/{parser}/{index}'] = run(port, sql)
        sql = (f"SELECT TOKENIZE(s,'{parser}') FROM (SELECT 'Alpha 中文' s UNION ALL "
               "SELECT 'Other 123' UNION ALL SELECT '' UNION ALL SELECT 'Alpha 中文') inputs")
        results[f'memo/rows/{parser}'] = run(port, sql)
    for index, sql in enumerate(("SELECT TOKENIZE('Alpha beta'),TOKENIZE('Alpha beta',NULL)",
                                "SELECT TOKENIZE('Alpha beta','space'),TOKENIZE('Alpha beta','space.1')",
                                "SELECT TOKENIZE('Alpha beta','beng'),TOKENIZE('Alpha beta','missing_parser')",
                                "SELECT TOKENIZE(NULL,'ik'),TOKENIZE('','ik')")):
        results[f'memo/identity/{index}'] = run(port, sql)
    texts = ['', ' ', 'THE Quick BROWN fox', 'lower lower LOWER', '123 １２３',
             '数据库全文索引，中文分词。', '中国人民银行数据库系统', '全文\n索引\t查询',
             'ＣＰＵ ＯｃｅａｎＢａｓｅ １２.３４', 'あいうえお カタカナ 한국어',
             '😀𠀀𪛖 A\x00B', 'école İSTANBUL Straße résumé',
             'a@b.c a-b a_b C++ C# 12,345.67', 'x' * 10000,
             ('词语重复 database 123，' * 1000)]
    rng = random.Random(20261029)
    alphabet = '汉字全文索引数据库AaZz09 ,.-_#\n\t😀éあ한'
    texts += [''.join(rng.choices(alphabet, k=80)) for _ in range(20)]
    raw_inputs = [text.encode() for text in texts] + [b'abc\xfftail', b'\xe4\xb8', b'\xc0\xaf']
    for parser in ('ik', 'beng', 'space', 'ngram', 'ngram2'):
        for mode in ('default', 'all'):
            for index, raw in enumerate(raw_inputs):
                sql = "SELECT TOKENIZE(CONVERT(X'%s' USING utf8mb4),'%s',JSON_ARRAY(JSON_OBJECT('output','%s')))" % (raw.hex(), parser, mode)
                result = run(port, sql)
                if index < len(texts):
                    assert result['returncode'] == 0, (parser, mode, index, result)
                # Decode JSON while preserving token-array order and all frequency fields.
                if result['returncode'] == 0:
                    result['output'] = json.loads(result['output'])
                results[f'tokens/{parser}/{mode}/{index}'] = result
    for parser, properties in [('ik', "JSON_OBJECT('ik_mode','smart')"),
                               ('ik', "JSON_OBJECT('ik_mode','max_word')"),
                               ('beng', "JSON_OBJECT('min_token_size',1),JSON_OBJECT('max_token_size',20)")]:
        for index in (2, 5, 6, 9, 12):
            sql = "SELECT TOKENIZE(CONVERT(X'%s' USING utf8mb4),'%s',JSON_ARRAY(JSON_OBJECT('output','all'),JSON_OBJECT('additional_args',JSON_ARRAY(%s))))" % (texts[index].encode().hex(), parser, properties)
            value = run(port, sql)
            assert value['returncode'] == 0, (sql, value)
            value['output'] = json.loads(value['output'])
            results[f'properties/{parser}/{properties}/{index}'] = value
    for collation in ('utf8mb4_bin', 'utf8mb4_general_ci', 'utf16_bin', 'utf16le_bin'):
        charset = collation.rsplit('_', 1)[0] if collation.endswith('_bin') else 'utf8mb4'
        for parser in ('ik', 'beng'):
            sql = "SELECT TOKENIZE(CONVERT(X'%s' USING %s) COLLATE %s,'%s')" % (
                'Mixed 全文 ＡＢＣ １２３'.encode(charset.replace('utf8mb4', 'utf-8').replace('utf16le', 'utf-16le').replace('utf16', 'utf-16be')).hex(),
                charset, collation, parser)
            results[f'charset/{collation}/{parser}'] = run(port, sql)
    queries = [("title, content", '全文索引 分词器'), ('title_en, content_en', 'database performance audit'),
               ('title, content', 'OceanBase 稳定 database'), ('content', '倒排索引 tokenizer'),
               ('content', '不存在的专用词 abcxyznomatch'), ('content', '数据库 数据库'),
               ('title_en, content_en', 'the and a')]
    for index, (columns, query) in enumerate(queries):
        match = f"MATCH({columns}) AGAINST('{query}')"
        results[f'match/ids/{index}'] = run(port, f'SELECT id FROM fts_large_bench.docs WHERE {match} ORDER BY id')
        results[f'match/rank/{index}'] = run(port, f'SELECT id,{match} FROM fts_large_bench.docs WHERE {match} ORDER BY 2 DESC,id LIMIT 40')
    setup = """DROP DATABASE IF EXISTS fts_hotpath_regression; CREATE DATABASE fts_hotpath_regression;
    CREATE TABLE fts_hotpath_regression.docs(id INT PRIMARY KEY, body TEXT,
      FULLTEXT INDEX ft(body) WITH PARSER beng);
    INSERT INTO fts_hotpath_regression.docs VALUES(1,'alpha beta'),(2,'alpha alpha'),(3,'gamma'),(4,NULL),(5,'');"""
    assert run(port, setup)['returncode'] == 0
    for stage, change in enumerate(['', "UPDATE fts_hotpath_regression.docs SET body='gamma' WHERE id=1",
                                   'DELETE FROM fts_hotpath_regression.docs WHERE id=2',
                                   "BEGIN; INSERT INTO fts_hotpath_regression.docs VALUES(6,'alpha'); ROLLBACK"]):
        if change:
            assert run(port, change)['returncode'] == 0
        value = run(port, "SELECT id FROM fts_hotpath_regression.docs WHERE MATCH(body) AGAINST('alpha') ORDER BY id")
        assert value['returncode'] == 0
        assert value['output'] == ['1\n2\n', '2\n', '', ''][stage], value
        results[f'dml/{stage}'] = value
    assert run(port, "INSERT INTO fts_hotpath_regression.docs VALUES(7,'alpha beta'),"
                     "(8,'alpha alpha beta'),(9,'beta'),(10,'alpha gamma')")['returncode'] == 0
    match = "MATCH(body) AGAINST('alpha beta')"
    other = "MATCH(body) AGAINST('gamma')"
    table = 'fts_hotpath_regression.docs'
    queries = [
        f'SELECT COUNT(*) FROM {table} WHERE {match}',
        f'SELECT COUNT(id),COUNT(NULL),SUM(id) FROM {table} WHERE {match}',
        f'SELECT id,{match} FROM {table} WHERE {match} ORDER BY id',
        f'SELECT id FROM {table} WHERE {match} > 0.5 ORDER BY id',
        f'SELECT id FROM {table} WHERE NOT ({match}) ORDER BY id',
        f'SELECT id FROM {table} WHERE ({match}) OR id=3 ORDER BY id',
        f'SELECT id FROM {table} WHERE ({match}) AND ({other}) ORDER BY id',
        f'SELECT id FROM {table} WHERE ({match}) OR ({other}) ORDER BY id',
        f'SELECT id FROM {table} WHERE {match} AND id % 2=0 ORDER BY id',
        f'SELECT id,{match} AS score FROM {table} WHERE {match} ORDER BY score DESC,id LIMIT 2',
        f'SELECT SUM({match}) FROM {table} WHERE {match}',
        f'SELECT id,{match} AS score FROM {table} HAVING score > 0.5 ORDER BY id',
        f"SELECT id FROM {table} WHERE MATCH(body) AGAINST('+alpha -gamma' IN BOOLEAN MODE) ORDER BY id",
        f'SELECT id FROM (SELECT id FROM {table} WHERE {match} LIMIT 2) v ORDER BY id',
        f'SELECT id FROM (SELECT id FROM {table} WHERE {match} LIMIT 3) v WHERE id=8 ORDER BY id',
        f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} ORDER BY {match} DESC LIMIT 2) v',
        f'SELECT COUNT(id) FROM (SELECT id FROM {table} WHERE {match} LIMIT 2) v',
        f'SELECT COUNT(score) FROM (SELECT {match} AS score FROM {table} WHERE {match} LIMIT 2) v',
        f'SELECT SUM(id) FROM (SELECT id FROM {table} WHERE {match} LIMIT 2) v',
        f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} LIMIT 3) v WHERE id=8',
        f'SELECT COUNT(*) FROM (SELECT DISTINCT body FROM {table} WHERE {match} LIMIT 2) v',
    ]
    for limit in (0, 1, 2, 10):
        for offset in (0, 1, 10):
            queries.append(f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} '
                           f'LIMIT {limit} OFFSET {offset}) v')
            queries.append(f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} '
                           f'AND id % 2=0 LIMIT {limit} OFFSET {offset}) v')
    for index, query in enumerate(queries):
        value = run(port, query)
        assert value['returncode'] == 0, (query, value)
        results[f'semantics/{index}'] = value
    long_terms = 'alpha beta ' + ' '.join(f'absentword{i:04d}' for i in range(300))
    for index, query_text in enumerate(('alpha beta gamma', long_terms)):
        query = f"SELECT COUNT(*) FROM {table} WHERE MATCH(body) AGAINST('{query_text}')"
        value = run(port, query)
        assert value['returncode'] == 0, (query, value)
        results[f'long-query/{index}'] = value
    prepared = (f"PREPARE fts_q FROM 'SELECT COUNT(*) FROM {table} WHERE MATCH(body) AGAINST(?)';"
                "SET @fts_term='alpha'; EXECUTE fts_q USING @fts_term;"
                "SET @fts_term='beta'; EXECUTE fts_q USING @fts_term;"
                "SET @fts_term='no_matching_word'; EXECUTE fts_q USING @fts_term;"
                "SET @fts_term='alpha'; EXECUTE fts_q USING @fts_term; DEALLOCATE PREPARE fts_q")
    results['prepared/query-reuse'] = run(port, prepared)
    assert results['prepared/query-reuse']['returncode'] == 0, results['prepared/query-reuse']
    for batch in (0, 1, 2, 7, 256):
        for index, predicate in enumerate((match, f"MATCH(body) AGAINST('{long_terms}')")):
            query = (f"SELECT /*+ opt_param('rowsets_max_rows',{batch}) */ COUNT(*) "
                     f"FROM {table} WHERE {predicate}")
            value = run(port, query)
            assert value['returncode'] == 0, (query, value)
            results[f'batch/{batch}/{index}'] = value
    ik_setup = """CREATE TABLE fts_hotpath_regression.ikdocs(id INT PRIMARY KEY, body TEXT,
      FULLTEXT INDEX ft(body) WITH PARSER ik);
      INSERT INTO fts_hotpath_regression.ikdocs VALUES
      (1,'数据库 全文索引'),(2,'数据库 分词器'),(3,'测试 文档')"""
    assert run(port, ik_setup)['returncode'] == 0
    ik_query = "SELECT id FROM fts_hotpath_regression.ikdocs WHERE MATCH(body) AGAINST('数据库') ORDER BY id"
    ik_changes = [('', '1\n2\n'),
                  ("INSERT INTO fts_hotpath_regression.ikdocs VALUES(4,'数据库 全文索引')", '1\n2\n4\n'),
                  ("UPDATE fts_hotpath_regression.ikdocs SET body='测试 文档' WHERE id=1", '2\n4\n'),
                  ('DELETE FROM fts_hotpath_regression.ikdocs WHERE id=4', '2\n'),
                  ("INSERT INTO fts_hotpath_regression.ikdocs VALUES(5,'数据库 全文索引')", '2\n5\n')]
    for stage, (change, expected) in enumerate(ik_changes):
        if change:
            assert run(port, change)['returncode'] == 0
        value = run(port, ik_query)
        assert value['returncode'] == 0 and value['output'] == expected, value
        results[f'ik-dml/{stage}'] = value
    transaction = ("BEGIN; UPDATE fts_hotpath_regression.ikdocs SET body='测试 文档' WHERE id=2;"
                   + ik_query + '; ROLLBACK; ' + ik_query)
    value = run(port, transaction)
    assert value['returncode'] == 0 and value['output'] == '5\n2\n5\n', value
    results['ik-dml/transaction-rollback'] = value
    assert run(port, 'DROP DATABASE fts_hotpath_regression')['returncode'] == 0
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=12886)
    parser.add_argument('--snapshot', type=Path, required=True)
    parser.add_argument('--compare', type=Path)
    args = parser.parse_args()
    observed = collect(args.port)
    args.snapshot.write_text(json.dumps(observed, ensure_ascii=False, indent=2) + '\n')
    if args.compare:
        previous = json.loads(args.compare.read_text())
        differences = [name for name in sorted(set(previous) | set(observed)) if previous.get(name) != observed.get(name)]
        if differences:
            raise SystemExit('FAIL: differing complete outputs: ' + ', '.join(differences))
    print(f'PASS: {len(observed)} complete token/query/DML outputs' + (' match baseline' if args.compare else ' captured'))
