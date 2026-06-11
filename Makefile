CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
TARGET ?= webserver
SRC = src/main.cpp

ifeq ($(OS),Windows_NT)
TARGET := webserver.exe
LIBS = -lws2_32
CLEAN_CMD = del /Q webserver.exe 2>NUL
else
LIBS = -pthread
CLEAN_CMD = rm -f webserver
endif

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LIBS)

run: $(TARGET)
	./$(TARGET) --port 8888 --threads 4 --max-connections 32 --root webroot

clean:
	$(CLEAN_CMD)
