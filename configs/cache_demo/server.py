from http.server import BaseHTTPRequestHandler, HTTPServer
import time

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = f"hello {time.time()}\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "public, max-age=60")
        self.end_headers()
        self.wfile.write(body)

HTTPServer(("127.0.0.1", 18080), Handler).serve_forever()