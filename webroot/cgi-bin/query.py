#!/usr/bin/env python3
import csv
import os
import sys
from pathlib import Path
from urllib.parse import parse_qs


def read_params():
    method = os.environ.get("REQUEST_METHOD", "GET").upper()
    if method == "POST":
        length = int(os.environ.get("CONTENT_LENGTH", "0") or "0")
        raw = sys.stdin.read(length)
    else:
        raw = os.environ.get("QUERY_STRING", "")
    return parse_qs(raw)


def find_student(student_id):
    root = Path(os.environ.get("DOCUMENT_ROOT", "."))
    data_file = root / "data" / "students.csv"
    with data_file.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row.get("student_id") == student_id:
                return row
    return None


params = read_params()
student_id = params.get("student_id", [""])[0].strip()
student = find_student(student_id) if student_id else None

print("Content-Type: text/html; charset=utf-8")
print()
print("""<!doctype html>
<html><head><meta charset="utf-8"><title>Student Query Result</title>
<link rel="stylesheet" href="/css/style.css"></head>
<body><main class="shell"><section class="panel">""")
if student:
    print("<h1>Student Found</h1>")
    print(f"<p>ID: <strong>{student['student_id']}</strong></p>")
    print(f"<p>Name: <strong>{student['name']}</strong></p>")
    print(f"<p>Class: <strong>{student['class']}</strong></p>")
else:
    print("<h1>Student Not Found</h1>")
    print(f"<p>No record for ID: <strong>{student_id or 'empty input'}</strong></p>")
print("""<div class="actions"><a href="/">Back Home</a></div>
</section></main></body></html>""")
