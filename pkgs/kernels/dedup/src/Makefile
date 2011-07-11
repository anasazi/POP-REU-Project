# Makefile for dedup kernel

PREFIX=${PARSECDIR}/pkgs/kernels/dedup/inst/${PARSECPLAT}

TARGET=dedup

BIGFILE_CFLAGS=$(shell getconf LFS_CFLAGS)

CFLAGS += -Wall -std=c99 -fno-strict-aliasing -D_XOPEN_SOURCE=600
CXXFLAGS += -Wall -fno-strict-aliasing -D_XOPEN_SOURCE=600
LIBS += -lcrypto -lz

DEDUP_OBJ =  hashtable.o queue.o util.o binheap.o tree.o decoder.o dedup.o rabin.o #encoder.o

ifdef version
  ifeq "$(version)" "pthreads"
    CFLAGS += -DPARALLEL -pthread
    COMPILER=$(CC)
    COMPILER_FLAGS=$(CFLAGS)
    DEDUP_OBJ += encoder.o
  endif
  ifeq "$(version)" "tbb_funcobj"
    CFLAGS += -DPARALLEL
    CXXFLAGS += -DPARALLEL
    COMPILER=$(CXX)
    COMPILER_FLAGS=$(CXXFLAGS)
    DEDUP_OBJ += encoder-funcobj.o
  endif
  ifeq "$(version)" "tbb_lambda"
    CFLAGS += -DPARALLEL
    CXXFLAGS += -DPARALLEL
    COMPILER=$(CXX)
    COMPILER_FLAGS=$(CXXFLAGS)
    DEDUP_OBJ += encoder-lambda.o
  endif
endif


all: $(TARGET)

.c.o:
#	$(CC) -c $(CFLAGS) $< -o $@
	$(COMPILER) -c $(COMPILER_FLAGS) $< -o $@

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $< -o $@

$(TARGET): $(DEDUP_OBJ)
	$(COMPILER) $(COMPILER_FLAGS) $(LDFLAGS) -o $(TARGET) $(DEDUP_OBJ) $(LIBS)
	#$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(DEDUP_OBJ) $(LIBS)

clean:
	rm -f *~ *.o $(TARGET)

install:
	mkdir -p $(PREFIX)/bin
	cp -f $(TARGET) $(PREFIX)/bin/$(TARGET)

