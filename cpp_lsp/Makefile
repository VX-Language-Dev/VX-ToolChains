CXX ?= clang++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
SRC_DIR = src
SOURCES = $(SRC_DIR)/json_utils.cpp $(SRC_DIR)/vx_lexer.cpp $(SRC_DIR)/lsp_server.cpp
TARGET = /tmp/vx-lsp-server
BUILD_DIR = /tmp/vx-build

all: $(TARGET)

$(TARGET): $(SOURCES)
	mkdir -p $(BUILD_DIR) && $(CXX) $(CXXFLAGS) -pipe -o $(TARGET) $(SOURCES) -I $(SRC_DIR)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	$(TARGET)

.PHONY: all clean run
