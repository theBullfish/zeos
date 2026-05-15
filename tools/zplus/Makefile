# zplus — Z+ compiler build
# Host build (Linux/Z13): produces a native binary for development.
# Kernel build: SRCS get compiled into BOOTZ.EFI via the parent Makefile.

CC      := gcc
CFLAGS  := -std=c99 -Wall -Wextra -Wpedantic -g -O2
LDFLAGS :=

SRCS := zplus.c lexer.c parser.c ast.c sfg.c ir.c codegen.c
HDRS := arena.h lexer.h ast.h parser.h sfg.h ir.h codegen.h zp_runtime.h
OBJS := $(SRCS:.c=.o)
BIN  := zplus

.PHONY: all clean test test-lex test-ast test-sfg test-smoke

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── tests ─────────────────────────────────────────────────────────
PROGRAMS := ../../programs

test: test-lex test-ast test-sfg test-smoke
	@echo "ALL TESTS PASSED"

# Lex the three smallest programs — must produce no errors
test-lex: $(BIN)
	@echo "--- lex: 01_file_watcher.zp"
	@./$(BIN) --dump-ast $(PROGRAMS)/01_file_watcher.zp > /dev/null && echo "  OK"
	@echo "--- lex: 50_strings_demo.zp"
	@./$(BIN) --dump-ast $(PROGRAMS)/50_strings_demo.zp > /dev/null && echo "  OK"
	@echo "--- lex: 51_struct_demo.zp"
	@./$(BIN) --dump-ast $(PROGRAMS)/51_struct_demo.zp > /dev/null && echo "  OK"

# Parse and dump AST — must produce non-empty output
test-ast: $(BIN)
	@echo "--- ast: 02_log_monitor.zp"
	@./$(BIN) --dump-ast $(PROGRAMS)/02_log_monitor.zp | grep -c "PIPELINE\|GATE\|SIGNAL_DECL" | \
		xargs -I{} sh -c 'test {} -gt 0 && echo "  OK ({})" || (echo "  FAIL: no nodes" && exit 1)'
	@echo "--- ast: kv-zeos.zp"
	@./$(BIN) --dump-ast $(PROGRAMS)/kv-zeos.zp | grep -c "CHAIN\|STRUCT\|IMPORT" | \
		xargs -I{} sh -c 'test {} -gt 0 && echo "  OK ({})" || (echo "  FAIL" && exit 1)'

# Lower to SFG — must produce chains and nodes
test-sfg: $(BIN)
	@echo "--- sfg: 51_struct_demo.zp"
	@./$(BIN) --dump-sfg $(PROGRAMS)/51_struct_demo.zp | grep -c "chain\|node\|SOURCE\|GATE" | \
		xargs -I{} sh -c 'test {} -gt 0 && echo "  OK ({})" || (echo "  FAIL" && exit 1)'
	@echo "--- sfg: 02_log_monitor.zp"
	@./$(BIN) --dump-sfg $(PROGRAMS)/02_log_monitor.zp | grep -c "GATE\|PROCESSOR" | \
		xargs -I{} sh -c 'test {} -gt 0 && echo "  OK ({})" || (echo "  FAIL" && exit 1)'

# Codegen smoke: compile three .zp files and check the output C compiles
test-smoke: $(BIN)
	@echo "--- smoke: 50_strings_demo.zp"
	@./$(BIN) --stdout $(PROGRAMS)/50_strings_demo.zp > /tmp/zp_smoke_50.c && \
		$(CC) -std=c99 -w -c -x c -I. -o /tmp/zp_smoke_50.o /tmp/zp_smoke_50.c && echo "  OK"
	@echo "--- smoke: 51_struct_demo.zp"
	@./$(BIN) --stdout $(PROGRAMS)/51_struct_demo.zp > /tmp/zp_smoke_51.c && \
		$(CC) -std=c99 -w -c -x c -I. -o /tmp/zp_smoke_51.o /tmp/zp_smoke_51.c && echo "  OK"
	@echo "--- smoke: 02_log_monitor.zp"
	@./$(BIN) --stdout $(PROGRAMS)/02_log_monitor.zp > /tmp/zp_smoke_02.c && \
		$(CC) -std=c99 -w -c -x c -I. -o /tmp/zp_smoke_02.o /tmp/zp_smoke_02.c && echo "  OK"

clean:
	rm -f $(OBJS) $(BIN) /tmp/zp_smoke_*.c
