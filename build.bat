@echo off
setlocal
g++ -std=c++17 -O2 -Wall -Wextra -o webserver.exe src\main.cpp -lws2_32
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
echo Built webserver.exe
