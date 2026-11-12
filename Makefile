CC ?= gcc
CFLAGS ?= -O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc
LDFLAGS ?=

LIBS = libfastmalloc.so libfastmalloc2.so libjitmalloc.so

all: $(LIBS) bench

bench: bench/micro bench/mt bench/stress bench/instr bench/check test_basic

src/size_classes.h: src/gen_classes.py
	python3 src/gen_classes.py src/size_classes.h

src/size_classes2.h: src/gen_classes2.py
	python3 src/gen_classes2.py src/size_classes2.h

libfastmalloc.so: src/alloc_v1.c src/size_classes.h
	$(CC) $(CFLAGS) -shared -o $@ src/alloc_v1.c $(LDFLAGS)

libfastmalloc2.so: src/fm_static.c src/fm_core.h src/size_classes2.h
	$(CC) $(CFLAGS) -shared -o $@ src/fm_static.c $(LDFLAGS)

libjitmalloc.so: src/fm_jit.c src/jit_stubs.S src/fm_core.h src/size_classes2.h
	$(CC) $(CFLAGS) -shared -o $@ src/fm_jit.c src/jit_stubs.S $(LDFLAGS)

bench/micro: bench/micro.c
	$(CC) -O2 -g -o $@ bench/micro.c -lpthread -ldl

bench/mt: bench/mt.c
	$(CC) -O2 -g -o $@ bench/mt.c -lpthread

bench/stress: bench/stress.c
	$(CC) -O2 -g -o $@ bench/stress.c -lpthread

bench/instr: bench/instr.c
	$(CC) -O2 -g -o $@ bench/instr.c

bench/check: bench/check.c
	$(CC) -O2 -g -o $@ bench/check.c

test_basic: test_basic.c
	$(CC) -O2 -g -o $@ test_basic.c

clean:
	rm -f $(LIBS) src/size_classes.h src/size_classes2.h \
      bench/micro bench/mt bench/stress bench/instr bench/check test_basic

.PHONY: all clean
