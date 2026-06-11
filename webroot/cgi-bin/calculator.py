#!/usr/bin/env python3
import os
import sys
from urllib.parse import parse_qs


def read_params():
    method = os.environ.get("REQUEST_METHOD", "GET").upper()
    if method == "POST":
        length = int(os.environ.get("CONTENT_LENGTH", "0") or "0")
        raw = sys.stdin.read(length)
    else:
        raw = os.environ.get("QUERY_STRING", "")
    return parse_qs(raw)


params = read_params()
try:
    a = float(params.get("a", ["0"])[0])
    b = float(params.get("b", ["0"])[0])
    op = params.get("op", ["sum"])[0]
    if op == "product":
        label = "Product"
        result = a * b
    else:
        label = "Sum"
        result = a + b
    body = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>Calculator Result</title>
<link rel="stylesheet" href="/css/style.css"></head>
<body><main class="shell"><section class="panel">
<h1>{label} Result</h1>
<p>{a} and {b} => <strong>{result}</strong></p>
<div class="actions"><a href="/">Back Home</a></div>
</section></main></body></html>"""
except Exception as exc:
    body = f"<html><body><h1>Bad Input</h1><p>{exc}</p></body></html>"

print("Content-Type: text/html; charset=utf-8")
print()
print(body)
