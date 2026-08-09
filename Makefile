NAME      := diskdoc

SRC_DIR   := src
INC_DIR   := include
TEST_DIR  := tests
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
BIN_DIR   := $(BUILD_DIR)/bin

CC       := gcc
CPPFLAGS := -I$(INC_DIR) -MMD -MP -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS  :=
LDLIBS   :=

ifeq ($(DEBUG),1)
  CFLAGS += -g3 -O0 -fsanitize=address,undefined
  LDFLAGS += -fsanitize=address,undefined
else
  CFLAGS += -O2
endif

SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

MAIN_OBJ := $(OBJ_DIR)/main.o
LIB_OBJS := $(filter-out $(MAIN_OBJ),$(OBJS))

TEST_SRCS := $(wildcard $(TEST_DIR)/*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/%.c,$(BIN_DIR)/%,$(TEST_SRCS))

TARGET   := $(BIN_DIR)/$(NAME)

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/%: $(TEST_DIR)/%.c $(LIB_OBJS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) -I$(TEST_DIR) $(CFLAGS) $< $(LIB_OBJS) $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: test
test: $(TEST_BINS)
	@if [ -z "$(TEST_BINS)" ]; then \
		echo "There is no suite in $(TEST_DIR)/"; \
		exit 0; \
	fi; \
	failed=0; total=0; \
	for t in $(TEST_BINS); do \
		total=$$((total + 1)); \
		echo "=== RUN  $$t"; \
		if ./$$t; then \
			echo "--- PASS $$t"; \
		else \
			echo "--- FAIL $$t (exit $$?)"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo "==========================================="; \
	echo "Total suite runned: $$total, fallite: $$failed"; \
	test $$failed -eq 0

.PHONY: run
run: $(TARGET)
	@./$(TARGET) $(ARGS)

.PHONY: clean
clean:
	$(RM) -r $(BUILD_DIR)

.PHONY: rebuild
rebuild: clean all

$(OBJ_DIR) $(BIN_DIR):
	@mkdir -p $@

-include $(OBJS:.o=.d)
-include $(TEST_BINS:=.d)
