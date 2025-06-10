# SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)

LIB_DIR = ../lib
include $(LIB_DIR)/common.mk

TARGET   := xdpact

LDLIBS += -lpthread -l:libact.a

LDFLAGS += -L$(LIB_DIR)/libact/src
CFLAGS += -I$(LIB_DIR)/libact/include




# Source and object files
SRC := xdpact.c
OBJ := $(SRC:.c=.o)

# Default target
all: $(TARGET)

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@


# Rule to create static library
$(TARGET): $(OBJ)
	$(CC) $^ $(LDLIBS) $(LDFLAGS)  -o $@

# Clean rule
.PHONY: clean
