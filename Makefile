# Makefile — xoraya-cli
# Build: make
# Clean: make clean
#
# Required system dependencies:
#   - X2E Linux SDK  : /usr/lib/libxorayasdk.so  /usr/include/x2e/
#   - g++ >= 9 (C++17 + std::filesystem built-in)
#   - pthread

CXX      ?= g++
CXXFLAGS  = -std=c++17 -Wall -Wextra -fPIC -O2
LDFLAGS   = -lxorayasdk -lpthread

TARGET    = xoraya-cli

# Sources
SRCS  = main.cpp
SRCS += scanner.cpp
SRCS += downloader.cpp
SRCS += collector.cpp

OBJS = $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
