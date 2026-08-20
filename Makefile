NAME      := diskdoc

PREFIX    := /usr/local
BINDIR    := $(PREFIX)/bin

SRC_DIR   := src
INC_DIR   := include
TP_DIR    := third_party
TEST_DIR  := tests
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
BIN_DIR   := $(BUILD_DIR)/bin

CC       := gcc
TP_INCS  := $(addprefix -I,$(wildcard $(TP_DIR)/*))
CPPFLAGS := -I$(INC_DIR) $(TP_INCS) -MMD -MP -D_POSIX_C_SOURCE=200809L
LDFLAGS  :=
LDLIBS   :=

BASE_CFLAGS := -std=c11

ifeq ($(DEBUG),1)
  BASE_CFLAGS += -g3 -O0 -fsanitize=address,undefined
  LDFLAGS += -fsanitize=address,undefined
else
  BASE_CFLAGS += -O2
endif

CFLAGS    := $(BASE_CFLAGS) -Wall -Wextra -Wpedantic
TP_CFLAGS := $(BASE_CFLAGS) -w

SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

TP_SRCS  := $(wildcard $(TP_DIR)/*/*.c)
TP_OBJS  := $(patsubst $(TP_DIR)/%.c,$(OBJ_DIR)/$(TP_DIR)/%.o,$(TP_SRCS))

MAIN_OBJ := $(OBJ_DIR)/main.o
LIB_OBJS := $(filter-out $(MAIN_OBJ),$(OBJS)) $(TP_OBJS)

TEST_SRCS := $(wildcard $(TEST_DIR)/*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/%.c,$(BIN_DIR)/%,$(TEST_SRCS))

TARGET   := $(BIN_DIR)/$(NAME)

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS) $(TP_OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/$(TP_DIR)/%.o: $(TP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TP_CFLAGS) -c $< -o $@

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
	echo "Total suites run: $$total, failed: $$failed"; \
	test $$failed -eq 0

.PHONY: run
run: $(TARGET)
	@./$(TARGET) $(ARGS)

.PHONY: clean
clean:
	$(RM) -r $(BUILD_DIR)

.PHONY: rebuild
rebuild: clean all

.PHONY: install
install: $(TARGET)
	install -Dm755 $(TARGET) $(BINDIR)/$(NAME)

.PHONY: uninstall
uninstall:
	$(RM) $(BINDIR)/$(NAME)

$(OBJ_DIR) $(BIN_DIR):
	@mkdir -p $@

-include $(OBJS:.o=.d)
-include $(TP_OBJS:.o=.d)
-include $(TEST_BINS:=.d)
