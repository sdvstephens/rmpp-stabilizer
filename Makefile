CC = aarch64-linux-gnu-g++
CFLAGS = -shared -fPIC -O2 -Wall -lm
SRC = src/stabilizer.cpp
OUT = libstabilizer.so

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(SRC) -o $(OUT) $(CFLAGS) -ldl

clean:
	rm -f $(OUT)

.PHONY: all clean
