# 推流端 Makefile(WSL 端)
# 用法: make          编译
#       make run      编译并启动推流
#       make clean    清理
CXX      ?= g++
CXXFLAGS  = -O2 -Wall
TARGET    = mjpeg_sender
SRCS      = mjpeg_sender.cpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
