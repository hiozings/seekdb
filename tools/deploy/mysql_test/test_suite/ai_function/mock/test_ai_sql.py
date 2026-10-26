#!/usr/bin/env python3
"""SQL/HTTP integration tests against a separately started seekdb instance.

AI_TEST_PORT=12882 python3 test_ai_sql.py. Requires mysql CLI only.
Creates and removes its own database/models; owns an in-process Mock on :8000.
"""
import json
import os
import subprocess
import threading
import unittest
from mock_ai_service import MockService, embedding

PORT = os.environ.get("AI_TEST_PORT", "12882")

def literal(value):
    return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"


class AIIntegration(unittest.TestCase):
    @classmethod
    def sql(cls, text, error=False):
        proc = subprocess.run(["mysql", "-h127.0.0.1", "-P" + PORT, "-uroot", "-N", "-B", "--raw",
                               "-e", "USE ai_enhancement_test; " + text], capture_output=True, text=True, timeout=30)
        if error:
            if proc.returncode == 0:
                raise AssertionError("expected SQL failure: " + text[:180])
            return proc.stderr
        if proc.returncode:
            raise AssertionError(proc.stderr + "\nSQL: " + text[:300])
        return proc.stdout.rstrip("\n")

    @classmethod
    def setUpClass(cls):
        cls.mock = MockService()
        cls.thread = threading.Thread(target=cls.mock.serve_forever, daemon=True)
        cls.thread.start()
        subprocess.run(["mysql", "-h127.0.0.1", "-P"+PORT, "-uroot", "-e",
                        "CREATE DATABASE IF NOT EXISTS ai_enhancement_test"], check=True)
        cls.models = []
        for name, kind, provider, path in [
            ("local_complete", "completion", "OPENAI", "/v1/chat/completions"),
            ("local_embed", "dense_embedding", "OPENAI", "/v1/embeddings"),
            ("local_rerank", "rerank", "SILICONFLOW", "/v1/rerank"),
        ]:
            cls.model(name, kind, provider, path)
        cls.sql("CREATE TABLE IF NOT EXISTS inputs(id INT PRIMARY KEY, content LONGTEXT); DELETE FROM inputs;" +
                "INSERT INTO inputs VALUES " + ",".join(f"({i},"+("NULL" if i % 5 == 0 else literal("文"+str(i)))+")" for i in range(1, 71)))

    @classmethod
    def model(cls, name, kind, provider, path, key="local-test"):
        for command in [f"DROP_AI_MODEL_ENDPOINT('{name}_ep')", f"DROP_AI_MODEL('{name}')"]:
            subprocess.run(["mysql", "-h127.0.0.1", "-P"+PORT, "-uroot", "-e", "CALL DBMS_AI_SERVICE."+command], capture_output=True)
        cls.sql(f"CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('{name}',"+literal(json.dumps({"type":kind,"model_name":"arbitrary-model"}))+ ")")

        if name not in cls.models:
            cls.models.append(name)
        config = {"ai_model_name":name,"provider":provider,"url":"http://127.0.0.1:8000"+path,"access_key":key}
        cls.sql(f"CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('{name}_ep',"+literal(json.dumps(config))+")")

    @classmethod
    def tearDownClass(cls):
        for name in cls.models:
            cls.sql(f"CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('{name}_ep'); CALL DBMS_AI_SERVICE.DROP_AI_MODEL('{name}');")
        cls.sql("DROP DATABASE ai_enhancement_test")
        requests=cls.mock.snapshot()
        batches=[len(r['body']['input']) for r in requests if isinstance(r['body'].get('input'),list)]
        print(f"Mock evidence: {len(requests)} requests, largest embedding input batch={max(batches,default=0)}",flush=True)
        cls.mock.shutdown()
        cls.mock.server_close()

    def complete(self, expression, config=""):
        return self.sql("SELECT AI_COMPLETE('local_complete',"+expression+config+")")

    def test_prompt_types_and_rendering(self):
        self.assertEqual("long-argument/long-argument/long-argument", self.complete("AI_PROMPT('{0}/{0}/{0}','long-argument')"))
        result = self.complete("AI_PROMPT('{0}|{1}', JSON_ARRAY(1,'世界'), JSON_OBJECT('a',2))")
        left, right = result.split('|')
        self.assertEqual([1,'世界'], json.loads(left))
        self.assertEqual({'a':2}, json.loads(right))
        self.assertEqual("outer inner 世界", self.complete("AI_PROMPT('outer {0}',AI_PROMPT('inner {0}','世界'))"))
        self.assertEqual("{1}", self.complete("AI_PROMPT('{0}','{1}')"))
        self.assertEqual("文1", self.sql("SELECT AI_COMPLETE('local_complete',AI_PROMPT(content)) FROM inputs WHERE id=1"))
        self.assertEqual(100000, int(self.sql("SELECT LENGTH(AI_COMPLETE('local_complete',AI_PROMPT('{0}',REPEAT('x',100000))))")))
        self.assertEqual('quote " slash \\ 世界', self.complete(literal('quote " slash \\ 世界')))
        for token in ['{2}', '{-1}', '{a}', '{999999999999999999999999}']:
            self.sql("SELECT AI_COMPLETE('local_complete',AI_PROMPT("+literal(token)+",'x'))", error=True)

    def test_completion_config(self):
        self.assertEqual("hello", self.complete("'hello'"))
        self.assertEqual("hello", self.complete("'hello'", ",NULL"))
        self.assertEqual("hello", self.complete("'hello'", ",JSON_OBJECT('temperature',0.25,'max_tokens',12)"))
        body = self.mock.snapshot()[-1]['body']
        self.assertEqual(0.25, body['temperature'])
        self.assertEqual(12, body['max_tokens'])
        self.assertEqual("hello", self.complete("'hello'", ",'{\"temperature\":0.5}'"))
        self.sql("SELECT AI_COMPLETE('local_complete','x',JSON_ARRAY(1))", error=True)

    def test_embedding_single(self):
        self.assertEqual(embedding('hello',4), json.loads(self.sql("SELECT AI_EMBED('local_embed','hello')")))
        self.assertEqual(embedding('hello',7), json.loads(self.sql("SELECT AI_EMBED('local_embed','hello',7)")))
        self.assertEqual('NULL', self.sql("SELECT AI_EMBED('local_embed',NULL)"))
        for dim in ['0','-1','1.2','NULL','18446744073709551615']:
            self.sql("SELECT AI_EMBED('local_embed','x',"+dim+")", error=True)

    def test_embedding_batch_mapping(self):
        before = len(self.mock.snapshot())
        output = self.sql("SELECT id,AI_EMBED('local_embed',content,3) FROM inputs ORDER BY id")
        for line in output.splitlines():
            key, value = line.split('\t'); key = int(key)
            self.assertEqual(None if key % 5 == 0 else embedding('文'+str(key),3), None if value == 'NULL' else json.loads(value))
        requests = self.mock.snapshot()[before:]
        self.assertTrue(any(len(r['body'].get('input',[])) > 1 for r in requests), requests)
        output = self.sql("SELECT id,AI_EMBED('local_embed',content,2) FROM inputs WHERE id%3=1 ORDER BY id LIMIT 11 OFFSET 2")
        for line in output.splitlines():
            key, value = line.split('\t'); key=int(key)
            self.assertEqual(None if key%5==0 else embedding('文'+str(key),2), None if value=='NULL' else json.loads(value))
        before = len(self.mock.snapshot())
        self.sql("SELECT AI_EMBED('local_embed',content) FROM inputs WHERE content IS NULL")
        self.assertEqual(before, len(self.mock.snapshot()))

    def test_rerank_strings_and_objects(self):
        docs = ['x'*i for i in range(1,42)]
        expected = sorted(range(len(docs)), key=lambda i: (-(len(docs[i])%7), i))
        result = json.loads(self.sql("SELECT AI_RERANK('local_rerank','q',"+literal(json.dumps(docs))+")"))
        self.assertEqual(expected,[r['index'] for r in result])
        objects = [{'id':100+i,'text':d} for i,d in enumerate(docs)]
        result = json.loads(self.sql("SELECT AI_RERANK('local_rerank','q',"+literal(json.dumps(objects))+",'text')"))
        self.assertEqual([100+i for i in expected],[r['id'] for r in result])

    def test_ollama(self):
        self.model('local_ollama','completion','oLlAmA','/api/generate','')
        self.assertEqual('hello',self.sql("SELECT AI_COMPLETE('local_ollama','hello')"))
        self.assertIs(False,self.mock.snapshot()[-1]['body']['stream'])
        self.model('local_ollama_embed','dense_embedding','OLLAMA','/api/embed','')
        self.assertEqual(embedding('hello',4),json.loads(self.sql("SELECT AI_EMBED('local_ollama_embed','hello')")))

    def test_transport_failures_and_recovery(self):
        for mode in ['http','json','disconnect','array','slow','large']:
            with self.subTest(mode=mode):
                self.model('local_transport','completion','OPENAI','/fault/'+mode+'/chat/completions')
                self.sql("SET ob_query_timeout=1000000; SELECT LENGTH(AI_COMPLETE('local_transport','hello'))",error=True)
                self.assertEqual('1',self.sql('SELECT 1'))
                self.assertEqual('ok',self.complete("'ok'"))

    def test_config_cannot_override_protocol_fields(self):
        for key in ['model','messages','stream']:
            self.sql("SELECT AI_COMPLETE('local_complete','hello',JSON_OBJECT("+literal(key)+",'override'))", error=True)

    def test_endpoint_alter_and_prepared_statement(self):
        self.model('local_alter','completion','OPENAI','/v1/chat/completions')
        config = literal(json.dumps({'provider':'oLlAmA','access_key':'','url':'http://127.0.0.1:8000/api/generate'}))
        result = self.sql("PREPARE stmt FROM \"SELECT AI_COMPLETE('local_alter','same')\"; EXECUTE stmt; " +
                          "CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('local_alter_ep',"+config+"); EXECUTE stmt; DEALLOCATE PREPARE stmt")
        self.assertEqual(['same','same'],result.splitlines())
        self.assertEqual('/api/generate',self.mock.snapshot()[-1]['path'])
        self.sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('local_alter_ep','{\"provider\":\"OPENAI\"}')",error=True)

    def test_multiple_batches_and_dimensions(self):
        self.sql("DELETE FROM inputs; INSERT INTO inputs VALUES "+','.join(f"({i},"+("NULL" if i%5==0 else literal('文'+str(i)))+")" for i in range(1,601)))
        before=len(self.mock.snapshot())
        output=self.sql("SELECT id,AI_EMBED('local_embed',content,2+id%3) FROM inputs ORDER BY id")
        self.assertEqual(600,len(output.splitlines()))
        for line in output.splitlines():
            key,value=line.split('\t'); key=int(key)
            self.assertEqual(None if key%5==0 else embedding('文'+str(key),2+key%3),None if value=='NULL' else json.loads(value))
        requests=self.mock.snapshot()[before:]
        self.assertTrue(len(requests)>3)
        self.assertTrue(all(1<=len(r['body']['input'])<=64 for r in requests))
        output=self.sql("SELECT id,CASE WHEN id%2=0 THEN AI_EMBED('local_embed',content,2) ELSE NULL END FROM inputs WHERE id<10 ORDER BY id")
        for line in output.splitlines():
            key,value=line.split('\t'); key=int(key)
            self.assertEqual(None if key%2 or key%5==0 else embedding('文'+str(key),2),None if value=='NULL' else json.loads(value))

    def test_dashscope_batch_limit_and_mapping(self):
        self.model('local_dash_embed','dense_embedding','ALIYUN-DASHSCOPE','/dashscope/embedding')
        before=len(self.mock.snapshot())
        output=self.sql("SELECT id,AI_EMBED('local_dash_embed',content,3) FROM inputs WHERE id<=21 ORDER BY id")
        for line in output.splitlines():
            key,value=line.split('\t'); key=int(key)
            self.assertEqual(None if key%5==0 else embedding('文'+str(key),3),None if value=='NULL' else json.loads(value))
        requests=self.mock.snapshot()[before:]
        self.assertTrue(len(requests)>=2)
        self.assertTrue(all(len(r['body']['input']['texts'])<=10 for r in requests))

    def test_second_batch_failure_and_recovery(self):
        self.model('local_second_fail','dense_embedding','OPENAI','/second_fail/embeddings')
        self.sql("DELETE FROM inputs; INSERT INTO inputs VALUES "+','.join(f"({i},'row{i}')" for i in range(1,151)))
        before=len(self.mock.snapshot())
        self.sql("SELECT AI_EMBED('local_second_fail',content,3) FROM inputs",error=True)
        requests=self.mock.snapshot()[before:]
        self.assertEqual(2,len(requests))
        self.assertTrue(all(len(r['body']['input'])>1 for r in requests))
        self.assertEqual(embedding('fresh',3),json.loads(self.sql("SELECT AI_EMBED('local_embed','fresh',3)")))

    def test_repeated_network_failures_and_recovery(self):
        self.model('local_repeat_fault','completion','OPENAI','/fault/json/chat/completions')
        for _ in range(30):
            self.sql("SELECT AI_COMPLETE('local_repeat_fault','x')",error=True)
        self.assertEqual('recovered',self.complete("'recovered'"))

    def test_bad_responses(self):
        for suffix, kind, provider, route, expr in [
            ('empty','completion','OPENAI','/empty/chat/completions',"AI_COMPLETE('local_fault','x')"),
            ('vector','dense_embedding','OPENAI','/bad_vector/embeddings',"AI_EMBED('local_fault','x')"),
            ('dim','dense_embedding','OPENAI','/bad_dim/embeddings',"AI_EMBED('local_fault','x',3)"),
            ('missing','rerank','SILICONFLOW','/missing/rerank',"AI_RERANK('local_fault','q','[\"a\",\"b\"]')"),
            ('duplicate','rerank','SILICONFLOW','/duplicate/rerank',"AI_RERANK('local_fault','q','[\"a\",\"b\"]')"),
        ]:
            with self.subTest(suffix=suffix):
                self.model('local_fault',kind,provider,route)
                self.sql('SELECT '+expr,error=True)
                self.assertEqual('1',self.sql('SELECT 1'))
                self.models.remove('local_fault')
                self.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('local_fault_ep'); CALL DBMS_AI_SERVICE.DROP_AI_MODEL('local_fault')")


if __name__ == '__main__':
    unittest.main(verbosity=2)
