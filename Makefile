# Makefile — xoraya-cli
# Build: make
# Clean: make clean
#
# Dépendances système requises :
#   - SDK X2E Linux  : /usr/lib/libxorayasdk.so  /usr/include/x2e/
#   - g++ >= 9 (C++17 + std::filesystem intégré)
#   - pthread

CXX      ?= g++
CXXFLAGS  = -std=c++17 -Wall -Wextra -fPIC -O2
LDFLAGS   = -lxorayasdk -lpthread

TARGET    = xoraya-cli

# Sources — on ajoute des fichiers au fil des phases
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
