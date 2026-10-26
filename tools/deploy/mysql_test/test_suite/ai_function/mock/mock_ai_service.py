#!/usr/bin/env python3
"""Deterministic local protocol fixture; never calls a real model.

Run standalone on port 8000, or import MockService in the SQL integration tests.
Faults are selected by URL, independently of model names and input content.
"""
import argparse
import hashlib
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def embedding(text, dimension):
    digest = hashlib.sha256(text.encode()).digest()
    return [round(digest[i % len(digest)] / 255, 6) for i in range(dimension)]


class MockService(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address=("0.0.0.0", 8000)):
        super().__init__(address, Handler)
        self.requests = []
        self.lock = threading.Lock()

    def snapshot(self):
        with self.lock:
            return list(self.requests)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.reply(200, self.server.snapshot() if self.path == "/requests" else {"status": "ok"})

    def reply(self, status, value):
        data = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when testing client cancellation/response limits.

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        with self.server.lock:
            self.server.requests.append({"path": self.path, "body": body})
        if self.path.startswith('/second_fail/'):
            with self.server.lock:
                calls = sum(r['path'] == self.path for r in self.server.requests)
            if calls == 2:
                return self.reply(400, {'error':'injected second-batch failure'})
        if self.path.startswith("/fault/array"):
            return self.reply(200, [])
        if self.path.startswith("/fault/slow"):
            time.sleep(2)
        if self.path.startswith("/fault/large"):
            return self.reply(200, {"choices": [{"message": {"content": "x" * (17 * 1024 * 1024)}}]})
        if self.path.startswith("/fault/http"):

            return self.reply(400, {"error": "injected"})
        if self.path.startswith("/fault/json"):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'{broken')
            return
        if self.path.startswith("/fault/disconnect"):
            self.close_connection = True
            return
        if self.path.endswith("/chat/completions") or self.path.endswith("/generate"):
            ollama = self.path.endswith("/generate")
            if ollama and body.get("stream") is not False:
                return self.reply(400, {"error": "stream must be false"})
            text = body.get("prompt") if ollama else body["messages"][-1]["content"]
            if "/empty/" in self.path:
                text = ""
            return self.reply(200, {"response": text} if ollama else {"choices": [{"message": {"content": text}}]})
        if self.path.endswith('/dashscope/embedding'):
            texts = body['input']['texts']
            if len(texts) > 10:
                return self.reply(400, {'error':'provider batch limit'})
            dim = body.get('parameters',{}).get('dimension',4)
            return self.reply(200, {'output':{'embeddings':list(reversed([
                {'text_index':i,'embedding':embedding(t,dim)} for i,t in enumerate(texts)]))}})
        if self.path.endswith("/embeddings") or self.path.endswith("/embed"):
            texts = body["input"]
            if not isinstance(texts, list) or not all(isinstance(t, str) for t in texts):
                return self.reply(400, {"error": "input must be a string array"})
            vectors = [embedding(t, body.get("dimensions", 4)) for t in texts]
            if "/bad_vector/" in self.path:
                vectors[0] = ["invalid"]
            if "/bad_dim/" in self.path:
                vectors[0].append(0)
            data = [{"index": i, "embedding": v} for i, v in enumerate(vectors)]
            if "/duplicate/" in self.path and len(data) > 1:
                data[1]["index"] = 0
            if "/missing/" in self.path:
                data = data[:-1]
            return self.reply(200, {"embeddings": vectors} if self.path.endswith("/embed") else {"data": list(reversed(data))})
        if self.path.endswith("/rerank"):
            docs = body["documents"]
            data = [{"index": i, "relevance_score": len(d) % 7} for i, d in enumerate(docs)]
            if "/duplicate/" in self.path and len(data) > 1:
                data[1]["index"] = 0
            if "/missing/" in self.path:
                data = data[:-1]
            return self.reply(200, {"results": list(reversed(data))})
        self.reply(404, {"error": "unknown route"})


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    MockService(("0.0.0.0", args.port)).serve_forever()
