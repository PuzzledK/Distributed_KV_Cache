CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -g -Iinclude
LDFLAGS = -pthread

SRC_DIR = src
OBJ_DIR = build

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))

TARGET = kv_cache
CLIENT_TARGET = kv_client

all: $(TARGET) $(CLIENT_TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

$(CLIENT_TARGET): client/client.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(CLIENT_TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run