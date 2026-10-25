CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -Iinclude -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
SRC      = src/main.c src/lines.c src/lcs.c src/moves.c src/tokens.c src/script.c src/bench.c src/tools.c
BIN      = adiff

all: $(BIN)

$(BIN): $(SRC) include/adiff.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: $(BIN)
	./$(BIN) selftest
	./$(BIN) --verify --stats examples/old.py examples/new.py || test $$? -eq 1

clean:
	rm -f $(BIN)

.PHONY: all test clean
