# =============================
# CGIL COMPILER FORGE MAKEFILE
# =============================

SHELL := cmd.exe

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra \
           -Iinclude/CodeGen \
           -Iinclude/Lexer \
           -Iinclude/Parser \
           -Iinclude/Semantics

TARGET = cgilc.exe
SRC_DIR = src
BUILD_DIR = build

SRCS = $(wildcard $(SRC_DIR)/*.cpp) \
       $(wildcard $(SRC_DIR)/CodeGen/*.cpp) \
       $(wildcard $(SRC_DIR)/Lexer/*.cpp) \
       $(wildcard $(SRC_DIR)/Parser/*.cpp) \
       $(wildcard $(SRC_DIR)/Semantics/*.cpp)

OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo [LINK] $@
	@$(CXX) $(CXXFLAGS) -o $@ $^
	@echo Success! Cgil engine forged.

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@if not exist $(subst /,\,$(dir $@)) mkdir $(subst /,\,$(dir $@))
	@echo [CXX] $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo [CLEAN] Removing build artifacts...
	@if exist $(BUILD_DIR) rmdir /s /q $(BUILD_DIR)
	@if exist $(TARGET) del /q $(TARGET)

.PHONY: all clean