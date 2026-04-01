CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
TARGET  = server
SRC     = server.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
