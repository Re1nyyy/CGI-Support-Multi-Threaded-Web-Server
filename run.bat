@echo off
setlocal
if not exist webserver.exe call build.bat
webserver.exe --port 8888 --threads 4 --max-connections 32 --root webroot --python python
