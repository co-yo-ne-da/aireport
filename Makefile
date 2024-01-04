CC=clang

LIBCURL = -lcurl
LIBJANSSON = -ljansson

CFLAGS = -Wall -O3 --std=c99

SRC_DIR=src
OBJ_DIR=obj
BIN_DIR=bin

SOURCES=$(wildcard $(SRC_DIR)/*.c)
OBJECTS=$(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SOURCES))

BIN = $(BIN_DIR)/aireport

all: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ ${LIBCURL} ${LIBJANSSON}

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) -c $< -o $@ $(CFLAGS)

clean:
	rm -r $(BIN_DIR)/* $(OBJ_DIR)/*

run:
	$(BIN)

asm:
	otool -tvV $(BIN)

install:
	cp $(BIN) /usr/local/bin
