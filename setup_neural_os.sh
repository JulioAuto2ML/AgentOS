#!/bin/bash

# Create root directory for NeuralOS
PROJECT_ROOT="neuralos"
mkdir -p "$PROJECT_ROOT"
cd "$PROJECT_ROOT"

# Function to create a directory and its parent directories if they don't exist
create_dir() {
    mkdir -p "$1"
    echo "Created directory: $1"
}

# Function to create an empty file if it doesn't exist
create_file() {
    touch "$1"
    echo "Created file: $1"
}

# Documentation directories
create_dir "docs/api"
create_dir "docs/arch"
create_dir "docs/perf"

# Header files
create_dir "include/kernel/mm"
create_dir "include/kernel/sched"
create_dir "include/kernel/sys"
create_dir "include/inference"
create_dir "include/common"

# Create header files
create_file "include/kernel/mm/memory_manager.h"
create_file "include/kernel/mm/slab.h"
create_file "include/kernel/mm/mmap.h"
create_file "include/kernel/mm/context.h"
create_file "include/kernel/sched/scheduler.h"
create_file "include/kernel/sched/priority.h"
create_file "include/kernel/sys/syscalls.h"
create_file "include/inference/engine.h"
create_file "include/inference/tensor.h"
create_file "include/inference/quantize.h"

# Source code directories
create_dir "src/kernel/mm"
create_dir "src/kernel/sched"
create_dir "src/kernel/sys"
create_dir "src/inference/simd"
create_dir "src/storage"
create_dir "src/monitor"
create_dir "src/init"

# Create source files
create_file "src/kernel/mm/slab.c"
create_file "src/kernel/mm/mmap.c"
create_file "src/kernel/mm/context.c"
create_file "src/kernel/mm/compact.c"
create_file "src/kernel/sched/scheduler.c"
create_file "src/kernel/sched/priority.c"
create_file "src/kernel/sched/load_balance.c"
create_file "src/kernel/sys/syscalls.c"
create_file "src/kernel/sys/ipc.c"
create_file "src/inference/engine.c"
create_file "src/inference/tensor.c"
create_file "src/inference/quantize.c"
create_file "src/inference/simd/sse.c"
create_file "src/inference/simd/avx.c"
create_file "src/storage/manager.c"
create_file "src/storage/cache.c"
create_file "src/storage/compress.c"
create_file "src/storage/io.c"
create_file "src/monitor/perf_monitor.c"
create_file "src/monitor/mem_monitor.c"
create_file "src/monitor/metrics.c"
create_file "src/init/main.c"
create_file "src/init/boot.c"

# Development tools
create_dir "tools/build"
create_dir "tools/debug"
create_dir "tools/benchmark"

# Test directories
create_dir "tests/unit/mm"
create_dir "tests/unit/sched"
create_dir "tests/unit/inference"
create_dir "tests/integration"
create_dir "tests/perf"

# Create test files
create_file "tests/unit/mm/memory_tests.c"
create_file "tests/unit/sched/scheduler_tests.c"
create_file "tests/unit/inference/engine_tests.c"
create_file "tests/perf/memory_bench.c"

# Scripts
create_dir "scripts/install"
create_dir "scripts/config"

# Third party dependencies
create_dir "third_party/llm"
create_dir "third_party/utils"

# Build output structure
create_dir "build/bin"
create_dir "build/lib"
create_dir "build/obj"
create_dir "build/tmp"

# Create root level files
create_file "Makefile"
create_file ".gitignore"
create_file "README.md"
create_file "LICENSE"

# Create initial .gitignore content
cat > .gitignore << 'EOL'
# Build directories
build/
*.o
*.so
*.a

# Editor files
.vscode/
.idea/
*.swp
*~

# OS specific
.DS_Store
.directory

# Binary files
*.bin
*.elf
*.img

# Debug files
*.debug
core.*

# Temporary files
*.tmp
*.temp
*.log
EOL

# Create initial Makefile content
cat > Makefile << 'EOL'
CC := gcc
AR := ar
KERNEL_SOURCES := $(wildcard src/kernel/*/*.c)
INFERENCE_SOURCES := $(wildcard src/inference/*.c)
STORAGE_SOURCES := $(wildcard src/storage/*.c)

CFLAGS := -O3 -march=native -mtune=native -msse4.2 -mavx2 -Wall -Wextra -I./include
LDFLAGS := -lpthread -lm -ldl

OBJECTS := $(KERNEL_SOURCES:.c=.o) $(INFERENCE_SOURCES:.c=.o) $(STORAGE_SOURCES:.c=.o)

all: neuralos

neuralos: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) neuralos
EOL

echo "NeuralOS project structure has been created successfully!"
