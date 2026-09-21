CC = gcc
LIB = libiio_wrapper_but_python_shouldnt_import_this.so
LIB_SRC = libiio_wrapper.c
LIB_OBJ = libiio_wrapper.o

# adc_capture: standalone user-space test/demo capture tool, independent
# of the libiio_wrapper.c shared library used by the Python/Qt UI.
BIN = adc_capture
BIN_SRC = adc_capture.c
BIN_OBJ = adc_capture.o

# adc_capture_view: terminal (ncurses) viewer/analyser for capture files
# produced by adc_capture; shares capture_format.h with adc_capture.
VIEW_BIN = adc_capture_view
VIEW_SRC = adc_capture_view.c
VIEW_OBJ = adc_capture_view.o

# adc_direct_capture: libIIO-free equivalent of adc_capture, talking to
# the kernel's IIO sysfs ABI and /dev/iio:deviceN directly. Intended to
# run only on the BeagleV-Fire target itself; deliberately has NO -liio
# dependency (only libc), unlike adc_capture.
DIRECT_BIN = adc_direct_capture
DIRECT_SRC = adc_direct_capture.c
DIRECT_OBJ = adc_direct_capture.o

CFLAGS = -g -O3 -Wall -fPIC
LDFLAGS = -shared -flto -liio
BIN_LDFLAGS = -liio
VIEW_LDFLAGS = -lncurses
DIRECT_LDFLAGS =

all: $(LIB) $(BIN) $(VIEW_BIN) $(DIRECT_BIN)

$(LIB): $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ -Wl,-soname,$@ $^

$(LIB_OBJ): $(LIB_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(BIN_OBJ)
	$(CC) -o $@ $^ $(BIN_LDFLAGS)

$(BIN_OBJ): $(BIN_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

$(VIEW_BIN): $(VIEW_OBJ)
	$(CC) -o $@ $^ $(VIEW_LDFLAGS)

$(VIEW_OBJ): $(VIEW_SRC) capture_format.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(DIRECT_BIN): $(DIRECT_OBJ)
	$(CC) -o $@ $^ $(DIRECT_LDFLAGS)

$(DIRECT_OBJ): $(DIRECT_SRC) capture_format.h
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: clean
clean:
	rm -f $(LIB_OBJ) $(BIN_OBJ) $(VIEW_OBJ) $(DIRECT_OBJ) $(LIB) $(BIN) $(VIEW_BIN) $(DIRECT_BIN)

