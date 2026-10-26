#!/usr/bin/env python3
"""Manage only this suite's private fixture directory and local socket policy."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
from generate import generate

def sql(socket, text):
    result=subprocess.run(['mysql','--socket='+socket,'-uroot','-N','-B','-e',text],capture_output=True,text=True,check=True)
    return result.stdout.strip()
def literal(value):return "'"+value.replace('\\','\\\\').replace("'","''")+"'"
def main(action,directory,socket):
    root=Path(directory)
    if action=='setup':
        if root.exists():raise RuntimeError('fixture directory must not exist')
        old=sql(socket,'SELECT @@GLOBAL.secure_file_priv')
        generate(root)
        (root/'.file-sql-fixture').write_text(json.dumps({'path':str(root),'policy':old}))
        sql(socket,'SET GLOBAL secure_file_priv='+literal(str(root)))
    elif action=='cleanup':
        if not root.exists():return
        if root.is_symlink():raise RuntimeError('refusing symlink fixture cleanup')
        state=json.loads((root/'.file-sql-fixture').read_text())
        if state['path']!=str(root):raise RuntimeError('fixture ownership mismatch')
        sql(socket,'SET GLOBAL secure_file_priv='+literal(state['policy']))
        shutil.rmtree(root)
    else:raise RuntimeError('unknown fixture action')
if __name__=='__main__':main(*sys.argv[1:])
