CC = gcc
LIB = libiio_wrapper_but_python_shouldnt_import_this.so
SRC = libiio_wrapper.c

CFLAGS = -g -O3 -c -fPIC
LDFLAGS = -shared -flto -liio

all: $(LIB)

$(LIB): *.o
	$(CC) $(LDFLAGS) -o $@ -Wl,-soname,$@ $?

*.o: $(SRC)
	$(CC) $(CFLAGS) $?

.PHONY: clean
clean:
	rm *.o
	rm $(LIB)
