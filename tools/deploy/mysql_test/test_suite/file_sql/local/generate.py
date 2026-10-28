#!/usr/bin/env python3
"""Generate format fixtures only; all queries in this suite execute in seekdb."""
from pathlib import Path
import argparse
import datetime
import shutil

def generate(destination):
    import pyarrow as pa
    import pyarrow.parquet as pq
    destination.mkdir(parents=True, exist_ok=True)
    source=Path(__file__).parent
    for name in ['basic.csv','basic.jsonl']:
        shutil.copyfile(source/name,destination/name)
    table=pa.table({'id':pa.array([1,2,3,4],type=pa.int64()),
                    'name':pa.array(['Alice',None,'C, D','Four']),
                    'score':pa.array([10.5,20.0,None,40.5],type=pa.float64()),
                    'active':pa.array([True,False,None,True]),
                    'day':pa.array([datetime.date(2024,2,29),None,datetime.date(2024,3,2),datetime.date(2024,3,3)],type=pa.date32()),
                    'time':pa.array([datetime.datetime(2024,2,29,12,30,15,123456)]*4,type=pa.timestamp('us'))})
    for version in ['1.0','2.0']:
        pq.write_table(table,destination/('basic'+version[0]+'.parquet'),compression='snappy',row_group_size=2,data_page_version=version,use_dictionary=True)
    pq.write_table(table,destination/'projection.parquet',compression={name:('snappy' if name=='id' else 'gzip') for name in table.column_names},row_group_size=2)
    (destination/'quotes.csv').write_bytes(b'id,text\r\n1,"a,b"\r\n2,"line1\nline2"\r\n3,"a""b"\r\n4,""\r\n5,\r\n')
    (destination/'bad.csv').write_text('a,b\n1,2,3\n')
    (destination/'nested.jsonl').write_text('{"a":{"b":1}}\n')
    (destination/'all_null.csv').write_text('value\n\n\n')
    (destination/'header_only.csv').write_text('id,name\n')
    (destination/'types.csv').write_text('n,d,b,s,date,time\n9223372036854775807,1.25,true,世界,2024-02-29,2024-02-29 12:30:00.123456\n-9223372036854775808,2.5,false,hello,2024-03-01,2024-03-01 00:00:00\n')
    with (destination/'large.csv').open('w') as f:
        f.write('id,value\n')
        for i in range(20000):f.write(f'{i},{i%13}\n')
    pq.write_table(pa.table({'id':pa.array(range(20000),type=pa.int64()),'payload':['x'*128]*20000}),destination/'large.parquet',row_group_size=2048,compression='snappy')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path);args=parser.parse_args();generate(args.directory)
