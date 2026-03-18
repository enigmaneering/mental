.PHONY: all lib clean test

# Detect platform
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LIB_EXT = dylib
    LIB_NAME = libmental.dylib
else ifeq ($(UNAME_S),Linux)
    LIB_EXT = so
    LIB_NAME = libmental.so
else ifeq ($(OS),Windows_NT)
    LIB_EXT = dll
    LIB_NAME = mental.dll
else
    $(error Unsupported platform: $(UNAME_S))
endif

# Default target
all: lib

# Ensure external libraries are present
external:
	@echo "Fetching external shader compilation libraries..."
	@go run fetch_external.go

# Build the shared library
lib: external
	@echo "Building Mental shared library for $(UNAME_S)..."
	@go build -buildmode=c-shared -o $(LIB_NAME) ./capi
	@cp capi/enigmatic.h mental.h
	@echo "✅ Built: $(LIB_NAME)"
	@echo "✅ Header: mental.h"

# Clean build artifacts
clean:
	@rm -f libmental.dylib libmental.so mental.dll mental.h
	@rm -rf external/
	@echo "Cleaned build artifacts"

# Run tests
test: external
	@go test ./...

# Build for specific platforms (for cross-compilation)
lib-darwin-arm64: external
	@GOOS=darwin GOARCH=arm64 go build -buildmode=c-shared -o libmental-darwin-arm64.dylib ./capi
	@cp capi/enigmatic.h mental.h
	@echo "✅ Built: libmental-darwin-arm64.dylib"

lib-darwin-amd64: external
	@GOOS=darwin GOARCH=amd64 go build -buildmode=c-shared -o libmental-darwin-amd64.dylib ./capi
	@cp capi/enigmatic.h mental.h
	@echo "✅ Built: libmental-darwin-amd64.dylib"

lib-linux-amd64: external
	@GOOS=linux GOARCH=amd64 go build -buildmode=c-shared -o libmental-linux-amd64.so ./capi
	@cp capi/enigmatic.h mental.h
	@echo "✅ Built: libmental-linux-amd64.so"

lib-linux-arm64: external
	@GOOS=linux GOARCH=arm64 go build -buildmode=c-shared -o libmental-linux-arm64.so ./capi
	@cp capi/enigmatic.h mental.h
	@echo "✅ Built: libmental-linux-arm64.so"

lib-windows-amd64: external
	@GOOS=windows GOARCH=amd64 go build -buildmode=c-shared -o mental-windows-amd64.dll ./capi
	@cp capi/enigmatic.h mental.h
	@echo "✅ Built: mental-windows-amd64.dll"

lib-windows-arm64: external
	@GOOS=windows GOARCH=arm64 go build -buildmode=c-shared -o mental-windows-arm64.dll ./capi
	@cp capi/enigmatic.h mental.h
	@echo "✅ Built: mental-windows-arm64.dll"

# Build all platforms (for releases)
lib-all: lib-darwin-arm64 lib-darwin-amd64 lib-linux-amd64 lib-linux-arm64 lib-windows-amd64 lib-windows-arm64
	@echo "✅ Built all platforms"
