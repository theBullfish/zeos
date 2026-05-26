# habana_proof — single-binary Linux MME proof for any Habana
# accelerator. No external deps beyond glibc and a kernel with the
# in-tree `habanalabs` module loaded.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c11
LDFLAGS ?=

TARGET  := habana_proof
SRCS    := habana_proof.c
HDRS    := habanalabs_uapi.h

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
