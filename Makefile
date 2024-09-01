CFLAGS := -std=c2x -Wall -Werror -pedantic-errors -D_DEFAULT_SOURCE -g
EXE := ./chttpd

all: ${EXE}

${EXE}: main.c
	clang ${CFLAGS} main.c -o ${EXE}

debug: ${EXE}
	clang -DDEBUG_MODE=1 ${CFLAGS} main.c -o ${EXE}
	${EXE}

run: ${EXE}
	${EXE}

stress:
	@# -n 100000000 => 100,000,000 requests total
	@# -t 500ms     => connections time out after 500ms.
	oha -n 100000000 -t 500ms --wait-ongoing-requests-after-deadline --disable-keepalive http://127.0.0.1:8080

clean:
	rm -f ${EXE}

.PHONY: clean debug
