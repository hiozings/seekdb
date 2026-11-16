#!/usr/bin/env python3
"""A real pending WAL is interrupted after a legal short pwrite, then replayed."""
import argparse
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from protocol_client import Session

class InterruptedRecovery(unittest.TestCase):
    def check_recovery(self, target, count, text):
        with tempfile.TemporaryDirectory(prefix='mvdb-recovery-') as directory:
            data = Path(directory)
            session = Session(BINARY, data)
            try:
                for sql in ['CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(4000))',
                            'INSERT INTO t VALUES '+','.join(f"({i},'{text}')" for i in range(count))]:
                    self.assertIs(session.execute(sql).get('ok'), True)
                proc = session.proc
                proc.kill()
                proc.wait(timeout=10)
                for stream in (proc.stdin, proc.stdout, proc.stderr): stream.close()
                session.proc = None
                self.assertGreater((data/'wal.log').stat().st_size, 0)
                env = os.environ.copy()
                env.update(LD_PRELOAD=str(LIBRARY), MVDB_TEST_INTERRUPT_FILE=target)
                proc = subprocess.Popen([str(BINARY),'--data-dir',str(data)],
                    stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
                try:
                    with selectors.DefaultSelector() as events:
                        events.register(proc.stderr, selectors.EVENT_READ)
                        self.assertTrue(events.select(15), 'recovery never reached the injected write')
                    self.assertEqual(proc.stderr.readline(), b'HALF_PAGE_WRITTEN\n')
                finally:
                    proc.kill()
                    proc.wait(timeout=10)
                    for stream in (proc.stdin, proc.stdout, proc.stderr): stream.close()
                self.assertGreater((data/'wal.log').stat().st_size, 0)
                session.start()
                response = session.execute('SELECT * FROM t ORDER BY id')
                self.assertEqual(response.get('rows'), [[i,text] for i in range(count)])
                response = session.execute(f"INSERT INTO t VALUES ({count-1},'duplicate')")
                self.assertIs(response.get('ok'), False, response)
                self.assertEqual(response['error']['code'], 'duplicate_key')
                session.restart()
                self.assertEqual(session.execute('SELECT id FROM t ORDER BY id')['rows'],
                                 [[i] for i in range(count)])
            finally:
                session.close()

    def test_checkpoint_publication_interrupted(self):
        # 5,000 overflow rows force a WAL >64 MiB and automatic checkpoint.
        # Interrupt early publication and the end of the heap write sequence.
        from unittest.mock import patch
        for nth in (1,10003):
            with self.subTest(write=nth), tempfile.TemporaryDirectory(prefix='mvdb-checkpoint-') as directory:
                data=Path(directory)
                session=Session(BINARY,data)
                self.assertIs(session.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(20000))')['ok'],True)
                self.assertIs(session.execute("INSERT INTO t VALUES(-1,'baseline')")['ok'],True)
                session.close()
                env={'MVDB_POOL_PAGES':'4','LD_PRELOAD':str(LIBRARY),
                     'MVDB_TEST_INTERRUPT_FILE':'.heap','MVDB_TEST_INTERRUPT_NTH':str(nth)}
                with patch.dict(os.environ,env):
                    session=Session(BINARY,data)
                try:
                    self.assertIn(b'MVDB_POOL_PAGES=4\0',Path(f'/proc/{session.proc.pid}/environ').read_bytes())
                    self.assertIs(session.execute('BEGIN')['ok'],True)
                    value='a'*9000
                    for start in range(0,5000,100):
                        sql='INSERT INTO t VALUES '+','.join(f"({i},'{value}')" for i in range(start,start+100))
                        self.assertIs(session.execute(sql)['ok'],True)
                    import json
                    session.proc.stdin.write(json.dumps({'id':99999,'op':'execute','sql':'COMMIT'})+'\n')
                    session.proc.stdin.flush()
                    proc=session.proc
                    try:
                        with selectors.DefaultSelector() as events:
                            events.register(proc.stderr,selectors.EVENT_READ)
                            self.assertTrue(events.select(45),'COMMIT never reached fault point')
                        self.assertEqual(proc.stderr.readline(),'HALF_PAGE_WRITTEN\n')
                        self.assertGreater((data/'wal.log').stat().st_size,64*1024*1024)
                    finally:
                        proc.kill();proc.wait(timeout=10)
                        for stream in (proc.stdin,proc.stdout,proc.stderr):stream.close()
                        session.proc=None
                    session.start()
                    self.assertEqual(session.execute('SELECT id FROM t ORDER BY id')['rows'],[[i] for i in range(-1,5000)])
                    for start in range(0,5000,100):
                        sql=f'SELECT id,s FROM t WHERE id>={start} AND id<{start+100} ORDER BY id'
                        self.assertEqual(session.execute(sql)['rows'],[[i,value] for i in range(start,start+100)])
                    self.assertEqual((data/'wal.log').stat().st_size,0)
                    self.assertIs(session.execute("INSERT INTO t VALUES(4999,'duplicate')")['ok'],False)
                finally:
                    session.close()

    def test_interrupted_heap_page(self):
        self.check_recovery('.heap', 2, 'x'*3500)

    def test_interrupted_btree_leaf(self):
        self.check_recovery('.btree', 400, 'x')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path, required=True)
    parser.add_argument('--fault-library', type=Path, required=True)
    args, rest = parser.parse_known_args()
    BINARY, LIBRARY = args.bin.resolve(), args.fault_library.resolve()
    unittest.main(argv=[sys.argv[0], *rest])
