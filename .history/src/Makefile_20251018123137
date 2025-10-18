CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2

all: minishell
minishell: minishell.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f minishell
.PHONY: all clean