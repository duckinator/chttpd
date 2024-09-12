CC ?= clang
CFLAGS := -std=c2x -Wall -Werror -pedantic-errors -D_DEFAULT_SOURCE -g ${CFLAGS}
EXE := ./chttpd

all: ${EXE}

${EXE}: main.c
	${CC} ${CFLAGS} main.c -o ${EXE}

msan:
	$(MAKE) clean all CFLAGS="-fsanitize=memory -fno-omit-frame-pointer"
	${EXE}

ubsan:
	$(MAKE) clean all CFLAGS="-fsanitize=undefined"
	${EXE}

run: ${EXE}
	${EXE}

stress:
	@# -n => total requests
	@# -t => connection timeout
	oha -n 100000000 -t 500ms --wait-ongoing-requests-after-deadline --disable-keepalive http://127.0.0.1:8080

slight-stress:
	oha -n 500000 -t 500ms --wait-ongoing-requests-after-deadline --disable-keepalive http://127.0.0.1:8080/

clean:
	rm -f ${EXE}

.PHONY: clean debug
