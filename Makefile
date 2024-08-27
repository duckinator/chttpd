CFLAGS := -std=c2x -Wall -Werror -pedantic-errors -D_POSIX_C_SOURCE=200809L -g
EXE := ./chttpd

all: ${EXE}

${EXE}: main.c
	clang ${CFLAGS} main.c -o ${EXE}

run: ${EXE}
	${EXE}

clean:
	rm -f ${EXE}

.PHONY: clean
