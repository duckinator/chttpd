CFLAGS := -std=c2x -Wall -Werror -pedantic-errors -D_POSIX_C_SOURCE=200809L
EXE := ./chttpd

all: ${EXE}

${EXE}:
	clang ${CFLAGS} main.c -o ${EXE}

run: ${EXE}
	${EXE}

clean:
	rm -f ${EXE}

.PHONY: clean
