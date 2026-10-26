#!/usr/bin/env python3
"""Run native mysqltest cases with isolated fixture setup/cleanup includes."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import sys

parser=argparse.ArgumentParser();parser.add_argument('--record',action='store_true');args=parser.parse_args()
suite=Path(__file__).resolve().parent;repo=Path(__file__).resolve().parents[5]
port=os.environ.get('FILE_SQL_TEST_PORT','12883')
socket=os.environ.get('FILE_SQL_TEST_SOCKET','/tmp/seekdb-file-sql-instance/run/sql.sock')
cases=[(case,suite/'r/mysql'/(case.stem+'.result')) for case in sorted((suite/'t').glob('*.test'))
       if case.name!='file_sql_basic.test']
# Self-built cases live in local/ so the official myqltest discovery stays clean.
cases+=[(case,case.with_suffix('.result')) for case in sorted((suite/'local').glob('*.test'))]
failed=[]
for case,result_file in cases:
    with tempfile.TemporaryDirectory(prefix='seekdb-native-file-tests-') as temporary:
        work=Path(temporary);data=work/'data'
        control=suite/'data/fixture_control.py'
        for action in ['setup','cleanup']:
            command=shlex.join([sys.executable,str(control),action,str(data),socket])
            (work/(action+'.inc')).write_text('--exec '+command+'\n')
        driver=work/'driver.test'
        driver.write_text(f'let $file_sql_data_dir={data};\nlet $file_sql_setup_include={work/"setup.inc"};\nlet $file_sql_cleanup_include={work/"cleanup.inc"};\n--source {case}\n')
        command=[str(repo/'deps/3rd/u01/obclient/bin/mysqltest'),'--host=127.0.0.1','--port='+port,'--user=root','--database=test','--test-file='+str(driver),'--result-file='+str(result_file),'--logdir='+str(work)]
        if args.record:command.append('--record')
        result=subprocess.run(command,cwd=repo/'tools/deploy',capture_output=True,text=True)
        print(case.name, 'PASS' if result.returncode==0 else 'FAIL',result.stdout,result.stderr,flush=True)
        if result.returncode:
            failed.append(case.name)
            for reject in work.glob('*.reject'):print(reject.read_text()[:8000],flush=True)
        if data.exists():subprocess.run([sys.executable,str(control),'cleanup',str(data),socket],check=True)
if failed:raise SystemExit('Failed cases: '+', '.join(failed))
