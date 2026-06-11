# CGI-Support Multi-Threaded Web Server

This repository contains a course project for **Network Programming Project 2: Building a CGI-Support Multi-Threaded Web Server**.

The server is implemented in C++17 with low-level TCP sockets. It supports static pages, CGI dynamic pages, a worker thread pool, max connection control, HTTP status handling, and access logging.

## Features

- Low-level TCP socket server without an HTTP framework.
- HTTP request parsing and HTTP response generation.
- Supports `GET`, `POST`, and `HEAD`.
- Returns `200`, `400`, `403`, `404`, and `500` status responses.
- Multi-threaded request processing with a worker thread pool.
- Configurable worker thread count from command-line arguments.
- Configurable maximum open connections.
- Static file service from `webroot`.
- CGI execution from `webroot/cgi-bin`.
- Access log at `webroot/log/server.log`.
- Windows and Linux compatible source code.

## Project Structure

```text
cgi_threaded_web_server/
  src/
    main.cpp
  webroot/
    index.html
    page1.html
    404.html
    css/
      style.css
    cgi-bin/
      calculator.py
      query.py
    data/
      students.csv
    log/
      .gitkeep
  Makefile
  build.bat
  run.bat
```

## Build

### Windows with MinGW

```powershell
.\build.bat
```

### Linux

```bash
make
```

## Run

### Windows PowerShell

```powershell
.\webserver.exe --port 8888 --threads 4 --max-connections 32 --root webroot --python python
```

### Linux

```bash
./webserver --port 8888 --threads 4 --max-connections 32 --root webroot --python python3
```

Open the browser:

```text
http://127.0.0.1:8888/
```

## Command-line Options

| Option | Default | Description |
|---|---:|---|
| `--port` | `8888` | Server listen port |
| `--threads` | `4` | Worker thread count |
| `--max-connections` | `32` | Maximum simultaneous open connections |
| `--root` | `webroot` | Web root directory |
| `--python` | `python` | Python interpreter used for CGI scripts |

## Test Examples

```powershell
Invoke-WebRequest -Uri http://127.0.0.1:8888/ -UseBasicParsing
Invoke-WebRequest -Uri http://127.0.0.1:8888/page1.html -Method Head -UseBasicParsing
Invoke-WebRequest -Uri http://127.0.0.1:8888/cgi-bin/calculator.py -Method Post -Body 'a=7&b=6&op=product' -ContentType 'application/x-www-form-urlencoded' -UseBasicParsing
Invoke-WebRequest -Uri http://127.0.0.1:8888/cgi-bin/query.py -Method Post -Body 'student_id=20260001' -ContentType 'application/x-www-form-urlencoded' -UseBasicParsing
```

## CGI Examples

- `calculator.py`: receives two numbers and returns their sum or product.
- `query.py`: receives a student ID and queries `webroot/data/students.csv`.

## Notes

This project intentionally implements HTTP handling over raw TCP sockets instead of using an existing web framework. It is designed for learning socket programming, HTTP message parsing, CGI execution, and multi-threaded server design.
