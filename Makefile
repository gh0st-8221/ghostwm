CC = gcc
CFLAGS = -Wall -Wextra -O3
LIBS = -lX11 -lXrandr -lXcomposite -lXdamage -lXrender -lm

ghostwm: ghostwm.c
	$(CC) $(CFLAGS) ghostwm.c -o ghostwm $(LIBS)

clean:
	rm -f ghostwm
