# Compiler and Flags
CC = gcc
CFLAGS = -Wall -g -std=c11
# -pthread for compiler, -lpthread for linker is more standard, but -pthread often works for both
PTHREAD_FLAGS = -pthread

# JSON-C Library
JSONC_LIBS = -ljson-c

# Math Library
MATH_LIBS = -lm

# SDL2 Library (for view)
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs)

# Source Files
SERVER_SRC = server.c list.c drone.c survivor.c
CLIENT_SRC = client.c drone.c
VIEW_SRC = view.c list.c drone.c survivor.c

# Executables
SERVER_EXE = server
CLIENT_EXE = client
VIEW_EXE = view

# Targets
all: $(SERVER_EXE) $(CLIENT_EXE) $(VIEW_EXE)

$(SERVER_EXE): $(SERVER_SRC)
	$(CC) $(CFLAGS) $(PTHREAD_FLAGS) $^ -o $@ $(JSONC_LIBS) $(MATH_LIBS)

$(CLIENT_EXE): $(CLIENT_SRC)
	$(CC) $(CFLAGS) $(PTHREAD_FLAGS) $^ -o $@ $(JSONC_LIBS) $(MATH_LIBS)

$(VIEW_EXE): $(VIEW_SRC)
	$(CC) $(CFLAGS) $(PTHREAD_FLAGS) $(SDL_CFLAGS) $^ -o $@ $(SDL_LIBS) $(JSONC_LIBS) $(MATH_LIBS)

# Run targets
run-server: $(SERVER_EXE)
	@echo "Starting server..."
	./$(SERVER_EXE)

# For running a single client, you might want to pass an ID
# Example: make run-client ARGS="D1"
# Or set a default:
CLIENT_ARGS ?= D1
run-client: $(CLIENT_EXE)
	@echo "Starting client with ID: $(CLIENT_ARGS)..."
	./$(CLIENT_EXE) $(CLIENT_ARGS)

run-view: $(VIEW_EXE)
	@echo "Starting view..."
	./$(VIEW_EXE)

# Target to start multiple drones (example for 3 drones)
# Example: make start-drones NUM_DRONES=5
NUM_DRONES ?= 3
start-drones: $(CLIENT_EXE)
	@echo "Starting $(NUM_DRONES) drone clients in the background..."
	@for i in $$(seq 1 $(NUM_DRONES)); do \
		echo "Starting drone D$$i"; \
		./$(CLIENT_EXE) D$$i & \
	done
	@echo "$(NUM_DRONES) drone clients launched. Check server logs and view."
	@echo "To stop them, you might need to use: pkill -f './$(CLIENT_EXE) D' or similar."

stop-drones:
	@echo "Attempting to stop drone clients..."
	@pkill -f "./$(CLIENT_EXE) D" || echo "No drone clients found running or pkill not available."

# Clean up
clean:
	@echo "Cleaning up compiled files..."
	rm -f $(SERVER_EXE) $(CLIENT_EXE) $(VIEW_EXE) *.o

# Phony targets are not files
.PHONY: all clean run-server run-client run-view start-drones stop-drones