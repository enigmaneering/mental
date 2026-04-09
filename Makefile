# Mental library build and test automation
.PHONY: all build test clean rebuild

# Detect MSYS2/MinGW and use the right generator
ifdef MSYSTEM
  CMAKE_GENERATOR := -G "MSYS Makefiles"
endif

# Default target: build and test
all: build test

# Build the library and tests
build:
	@echo "Building mental library..."
	@cmake -B build $(CMAKE_GENERATOR)
	@cmake --build build --parallel

# Run tests
test: build
	@echo "Running tests..."
	@cd build && ctest --output-on-failure

# Clean build artifacts
clean:
	@echo "Cleaning build directory..."
	@rm -rf build

# Rebuild from scratch
rebuild: clean all
