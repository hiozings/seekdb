#!/usr/bin/env python3
"""Run real seekdb SQL against isolated local fixtures; no external SQL engine."""
import datetime
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest

PORT=os.environ.get('FILE_SQL_TEST_PORT','12883')
def quote(value):return "'"+str(value).replace('\\','\\\\').replace("'","''")+"'"
class FileSQL(unittest.TestCase):
    @classmethod
    def sql(cls,text,error=False,enabled=True,local=False):
        prefix='SET enable_file_sql=ON; ' if enabled else ''
        connection=['--socket='+os.environ.get('FILE_SQL_TEST_SOCKET','/tmp/seekdb-file-sql-instance/run/sql.sock')] if local else ['-h127.0.0.1','-P'+PORT]
        result=subprocess.run(['mysql',*connection,'-uroot','-N','-B','--raw','-e',prefix+text],capture_output=True,text=True,timeout=45)
        if error:
            if result.returncode==0:raise AssertionError('expected failure: '+text[:180])
            if 'ERROR 2013' in result.stderr or 'ERROR 2006' in result.stderr or 'ERROR 2002' in result.stderr or 'ERROR 2003' in result.stderr:raise AssertionError('server connection lost: '+result.stderr)
            return result.stderr
        if result.returncode:raise AssertionError(result.stderr+'\nSQL: '+text[:250])
        return result.stdout.rstrip('\n')
    @classmethod
    def setUpClass(cls):
        cls.directory=tempfile.TemporaryDirectory(prefix='seekdb-file-sql-')
        cls.root=Path(cls.directory.name)
        spec=importlib.util.spec_from_file_location('fixtures',Path(__file__).parent/'local/generate.py')
        fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures);fixtures.generate(cls.root)
        cls.previous_root=cls.sql('SELECT @@GLOBAL.secure_file_priv',enabled=False,local=True)
        cls.sql('SET GLOBAL secure_file_priv='+quote(cls.root),enabled=False,local=True)
    @classmethod
    def tearDownClass(cls):
        cls.sql('SET GLOBAL secure_file_priv='+quote(cls.previous_root),enabled=False,local=True)
        cls.directory.cleanup()
    def path(self,name):return quote(self.root/name)
    def test_default_off_and_security(self):
        self.sql('SELECT * FROM read_csv('+self.path('basic.csv')+')',error=True,enabled=False)
        for path in ['/etc/passwd',str(self.root/'../outside.csv')]:
            self.sql('SELECT * FROM read_csv('+quote(path)+')',error=True)
        link=self.root/'link.csv';link.symlink_to('/etc/passwd')
        self.sql('SELECT * FROM read_csv('+quote(link)+')',error=True);link.unlink()
        fifo=self.root/'fifo.csv';os.mkfifo(fifo)
        self.sql('SELECT * FROM read_csv('+quote(fifo)+')',error=True);fifo.unlink()
    def test_csv_native_query(self):
        table='read_csv('+self.path('basic.csv')+')'
        self.assertEqual([3,30,15,10,20],list(map(float,self.sql('SELECT COUNT(*),SUM(score),AVG(score),MIN(score),MAX(score) FROM '+table).split('\t'))))
        self.assertEqual('Bob\t21',self.sql('SELECT name,score+1 AS next_score FROM '+table+' WHERE id>1 ORDER BY score DESC LIMIT 1'))
        self.assertEqual('1\t2',self.sql('SELECT active,COUNT(*) FROM '+table+' GROUP BY active HAVING COUNT(*)>1'))
        self.assertIn('FILE_SCAN',self.sql('EXPLAIN SELECT id FROM '+table))
    def test_csv_options_null_and_quotes(self):
        table='read_csv('+self.path('quotes.csv')+',header=>true,sample_size=-1)'
        self.assertEqual('5\t4',self.sql('SELECT COUNT(*),COUNT(text) FROM '+table))
        self.assertEqual('a,b\na"b',self.sql('SELECT text FROM '+table+' WHERE id IN (1,3) ORDER BY id'))
        self.assertEqual('11',self.sql('SELECT LENGTH(text) FROM '+table+' WHERE id=2'))
        self.assertEqual('0',self.sql('SELECT LENGTH(text) FROM '+table+' WHERE id=4'))
        self.sql('SELECT * FROM read_csv('+self.path('basic.csv')+',sample_size=2)',error=True)
        self.sql('SELECT * FROM read_csv('+self.path('basic.csv')+',header=true,header=false)',error=True)
    def test_json_union_and_alias(self):
        table='read_ndjson('+self.path('basic.jsonl')+')'
        self.assertEqual('1\tNULL\n3\t1\n4\tNULL',self.sql('SELECT id,extra FROM '+table+' ORDER BY id'))
        self.assertEqual('3\t2\t3.5',self.sql('SELECT COUNT(*),COUNT(amount),SUM(amount) FROM read_json('+self.path('basic.jsonl')+')'))
        self.assertEqual('third',self.sql('SELECT j.label AS value FROM '+table+' AS j WHERE j.id=3'))
    def test_parquet_types_groups_and_projection(self):
        for version in ['1','2']:
            table='read_parquet('+self.path('basic'+version+'.parquet')+')'
            self.assertEqual('4\t3\t71',self.sql('SELECT COUNT(*),COUNT(score),SUM(score) FROM '+table))
            self.assertEqual('1\tAlice\n2\tNULL\n3\tC, D\n4\tFour',self.sql('SELECT id,name FROM '+table+' ORDER BY id'))
            self.assertEqual('2024-02-29',self.sql('SELECT day FROM '+table+' WHERE id=1'))
            self.assertIn('123456',self.sql('SELECT time FROM '+table+' WHERE id=1'))
        self.assertEqual('10',self.sql('SELECT SUM(id) FROM read_parquet('+self.path('projection.parquet')+')'))
        self.sql('SELECT name FROM read_parquet('+self.path('projection.parquet')+')',error=True)
    def test_native_joins_and_rescan(self):
        csv='read_csv('+self.path('basic.csv')+')';json='read_json('+self.path('basic.jsonl')+')';parquet='read_parquet('+self.path('basic1.parquet')+')'
        self.assertEqual('1\tfirst\n2\tNULL\n3\tthird',self.sql('SELECT a.id,b.label FROM '+csv+' a LEFT JOIN '+json+' b ON a.id=b.id ORDER BY a.id'))
        self.assertEqual('3',self.sql('SELECT COUNT(*) FROM '+csv+' a INNER JOIN '+parquet+' b ON a.id=b.id'))
        self.assertEqual('3',self.sql('SELECT /*+ USE_NL(a b) */ COUNT(*) FROM '+csv+' a JOIN '+csv+' b ON a.id=b.id'))
        self.assertEqual('3',self.sql('SELECT COUNT(*) FROM '+csv+' a JOIN '+parquet+' b USING(id)'))
        plan=self.sql('EXPLAIN SELECT a.id FROM '+csv+' a JOIN '+json+' b ON a.id=b.id')
        self.assertIn('JOIN',plan);self.assertGreaterEqual(plan.count('FILE_SCAN'),2)
    def test_describe_and_glob(self):
        description=self.sql('DESCRIBE SELECT id,name,score FROM read_csv('+self.path('basic.csv')+')')
        self.assertEqual(['id','name','score'],[line.split('\t')[0] for line in description.splitlines()])
        paths=self.sql('SELECT file FROM glob('+self.path('basic*.csv')+')')
        self.assertEqual(str(self.root/'basic.csv'),paths)
        self.assertEqual('0',self.sql('SELECT COUNT(*) FROM glob('+self.path('absent*.csv')+')'))
    def test_types_null_and_empty(self):
        self.assertEqual('9223372036854775807\n-9223372036854775808',self.sql('SELECT n FROM read_csv('+self.path('types.csv')+')'))
        self.assertEqual('2\t0',self.sql('SELECT COUNT(*),COUNT(value) FROM read_csv('+self.path('all_null.csv')+')'))
        self.assertEqual('0',self.sql('SELECT COUNT(*) FROM read_csv('+self.path('header_only.csv')+')'))
    def test_bad_records_and_ordinary_sql(self):
        self.sql('SELECT * FROM read_csv('+self.path('bad.csv')+')',error=True)
        self.sql('SELECT * FROM read_json('+self.path('nested.jsonl')+')',error=True)
        self.assertEqual('2\t普通 SQL',self.sql("SELECT 1+1,CONCAT('普通',' SQL')",enabled=False))
    def test_multi_batch(self):
        for name,fn in [('large.csv','read_csv'),('large.parquet','read_parquet')]:
            self.assertEqual('20000\t199990000',self.sql('SELECT COUNT(*),SUM(id) FROM '+fn+'('+self.path(name)+')'))
            self.assertEqual('100\n101\n102',self.sql('SELECT id FROM '+fn+'('+self.path(name)+') WHERE id>=100 ORDER BY id LIMIT 3'))
    def test_copy_roundtrip_and_no_overwrite(self):
        destination=self.root/'copy.csv'
        self.sql('COPY (SELECT id,name,score FROM read_csv('+self.path('basic.csv')+') ORDER BY id) TO '+quote(destination)+' (FORMAT CSV, HEADER true)')
        self.assertTrue(destination.exists())
        self.assertEqual('3\t30',self.sql('SELECT COUNT(*),SUM(score) FROM read_csv('+quote(destination)+')'))
        original=destination.read_bytes()
        self.sql('COPY (SELECT 1) TO '+quote(destination),error=True)
        self.assertEqual(original,destination.read_bytes())
        custom=self.root/'custom.csv'
        self.sql('COPY (SELECT id,name,score FROM read_csv('+self.path('basic.csv')+')) TO '+quote(custom)+" (HEADER true, DELIMITER '|', NULL 'nil')")
        self.assertEqual('3\t2',self.sql('SELECT COUNT(*),COUNT(score) FROM read_csv('+quote(custom)+",delimiter='|',nullstr='nil')"))
        with tempfile.TemporaryDirectory(prefix='file-sql-outside-') as outside:
            self.sql('COPY (SELECT 1) TO '+quote(Path(outside)/'forbidden.csv'),error=True)
            self.assertFalse((Path(outside)/'forbidden.csv').exists())
    def test_copy_invalid_utf8_and_query_failure(self):
        destination=self.root/'invalid-utf8.csv'
        self.sql("COPY (SELECT UNHEX('ff') AS x) TO "+quote(destination),error=True)
        self.assertFalse(destination.exists())
        self.assertEqual('1',self.sql('SELECT 1'))
        destination=self.root/'query-failed.csv'
        self.sql("COPY (SELECT JSON_EXTRACT(IF(id=2,'bad','{\"a\":1}'),'$.a') AS x FROM read_csv("+self.path('basic.csv')+")) TO "+quote(destination),error=True)
        self.assertFalse(destination.exists())
        self.assertFalse(list(self.root.glob('.seekdb-copy-*')))

    def test_copy_table_and_scalar_roundtrip(self):
        name='test.file_sql_copy_fixture'
        self.sql('CREATE DATABASE IF NOT EXISTS test; CREATE TABLE '+name+' (id BIGINT, value DOUBLE, text VARCHAR(100))')
        try:
            self.sql("INSERT INTO "+name+" VALUES (1,1.2345678901234567,'a,b'),(2,NULL,'世界')")
            target=self.root/'table-copy.csv'
            self.sql('COPY '+name+' TO '+quote(target))
            self.assertEqual('1',self.sql('SELECT COUNT(*) FROM read_csv('+quote(target)+') a JOIN '+name+' b ON a.id=b.id AND a.value=b.value'))
            self.assertEqual('2',self.sql('SELECT COUNT(*) FROM read_csv('+quote(target)+')'))
        finally:self.sql('DROP TABLE '+name)

    def test_copy_policy_change_and_cleanup(self):
        target=self.root/'policy-copy.csv'
        def revoke():
            time.sleep(0.4)
            self.sql("SET GLOBAL secure_file_priv='NULL'",enabled=False,local=True)
        thread=threading.Thread(target=revoke);thread.start()
        try:
            self.sql('COPY (SELECT id+SLEEP(0.0001) AS id FROM read_csv('+self.path('large.csv')+')) TO '+quote(target),error=True)
        finally:
            thread.join()
            self.sql('SET GLOBAL secure_file_priv='+quote(self.root),enabled=False,local=True)
        self.assertFalse(target.exists())
        self.assertFalse(list(self.root.glob('.seekdb-copy-*')))

    @unittest.skipUnless(os.environ.get('FILE_SQL_RESOURCE_TEST')=='1','set FILE_SQL_RESOURCE_TEST=1 for resource measurement')
    def test_streaming_memory_and_descriptors(self):
        socket=Path(os.environ.get('FILE_SQL_TEST_SOCKET','/tmp/seekdb-file-sql-instance/run/sql.sock'))
        pid=int((socket.parent/'seekdb.pid').read_text())
        def resident():
            return int(next(line.split()[1] for line in Path('/proc/'+str(pid)+'/status').read_text().splitlines() if line.startswith('VmRSS:')))
        def descriptors():return len(list(Path('/proc/'+str(pid)+'/fd').iterdir()))
        path=self.root/'resource.csv'
        with path.open('w') as output:
            output.write('id,payload\n')
            for i in range(200000):output.write(str(i)+','+'x'*1024+'\n')
        baseline=resident();peak=[baseline];stop=threading.Event()
        def sample():
            while not stop.wait(0.02):peak[0]=max(peak[0],resident())
        thread=threading.Thread(target=sample);thread.start()
        try:self.assertEqual('200000\t19999900000\t1024',self.sql('SELECT COUNT(*),SUM(id),MAX(LENGTH(payload)) FROM read_csv('+quote(path)+')'))
        finally:stop.set();thread.join()
        before=descriptors()
        for _ in range(20):
            self.sql('SELECT COUNT(*) FROM read_csv('+self.path('basic.csv')+')')
            self.sql('SELECT * FROM read_csv('+self.path('bad.csv')+')',error=True)
        self.sql('SET ob_query_timeout=10000; SELECT * FROM read_csv('+quote(path)+')',error=True)
        after=descriptors()
        print(f'File SQL resource evidence: input={path.stat().st_size} bytes, RSS delta={peak[0]-baseline} KiB, FDs {before}->{after}',flush=True)
        self.assertLess(peak[0]-baseline,128*1024)
        self.assertLessEqual(after,before+8)

    def test_file_change_mid_scan(self):
        path=self.root/'changing.csv';path.write_text('id\n'+''.join(str(i)+'\n' for i in range(20000)))
        def mutate():
            time.sleep(0.4)
            with path.open('a') as f:f.write('99999\n')
        thread=threading.Thread(target=mutate);thread.start()
        try:self.sql('SELECT SUM(id+SLEEP(0.0001)) FROM read_csv('+quote(path)+')',error=True)
        finally:thread.join()
        self.assertEqual('20001',self.sql('SELECT COUNT(*) FROM read_csv('+quote(path)+')'))

if __name__=='__main__':unittest.main(verbosity=2)
