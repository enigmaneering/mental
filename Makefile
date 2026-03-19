# Mental library build and test automation
.PHONY: all build test clean rebuild

# Default target: build and test
all: build test

# Build the library and tests
build:
	@echo "Building mental library..."
	@mkdir -p build
	@cd build && cmake .. && $(MAKE)

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
