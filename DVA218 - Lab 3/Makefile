CC = gcc
CFLAGS = -g
LFLAGS = -lssl -lcrypto -pthread
PROGRAMS = main

ALL: ${PROGRAMS}

main: main.c
	${CC}  ${CFLAGS}  -o main main.c utp.c ${LFLAGS}

clean:
	rm -f ${PROGRAMS}
