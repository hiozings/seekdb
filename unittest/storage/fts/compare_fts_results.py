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
    for parser in ('ik', 'beng'):
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
