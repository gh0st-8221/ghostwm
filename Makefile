CC = gcc
CFLAGS = -Wall -Wextra -O3
LIBS = -lX11 -lXrandr
TARGET = ghostwm

all: $(TARGET)

$(TARGET): ghostwm.c config.h
	$(CC) $(CFLAGS) ghostwm.c -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
