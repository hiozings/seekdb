#!/usr/bin/env python3
"""Protocol fixture for official AI function cases; all models default to 768 dimensions.

No SQL or expected results are interpreted here. Requests are retained for audit.
"""
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Service(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address=('0.0.0.0', 8000)):
        super().__init__(address, Handler)
        self.requests = []
        self.lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, value):
        data = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        with self.server.lock:
            self.server.requests.append({'path': self.path, 'body': body})
        if self.path.endswith('/chat/completions'):
            return self.reply(200, {'choices': [{'message': {'content': body['messages'][-1]['content']}}]})
        if self.path.endswith('/generate'):
            if body.get('stream') is not False:
                return self.reply(400, {'error': 'stream must be false'})
            return self.reply(200, {'response': body['prompt']})
        if self.path.endswith('/embeddings') or self.path.endswith('/embed'):
            texts = body['input']
            if not isinstance(texts, list) or not all(isinstance(t, str) for t in texts):
                return self.reply(400, {'error': 'input must be a string array'})
            dimension = body.get('dimensions', 768)
            vectors = [[round(hashlib.sha256(t.encode()).digest()[i % 32] / 255, 6)
                        for i in range(dimension)] for t in texts]
            if self.path.endswith('/embed'):
                return self.reply(200, {'embeddings': vectors})
            return self.reply(200, {'data': list(reversed([
                {'index': i, 'embedding': v} for i, v in enumerate(vectors)]))})
        if self.path.endswith('/rerank'):
            query = set(body['query'].lower().split())
            return self.reply(200, {'results': list(reversed([
                {'index': i, 'relevance_score': len(query & set(text.lower().split()))}
                for i, text in enumerate(body['documents'])]))})
        self.reply(404, {'error': 'unknown route'})
