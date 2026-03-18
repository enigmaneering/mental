
package transpile

/*
#cgo CPPFLAGS: -I${SRCDIR}/../external/glslang
#cgo CPPFLAGS: -I${SRCDIR}/../external/glslang/glslang/Public
#cgo !windows CXXFLAGS: -std=c++11
#cgo windows CXXFLAGS: -std=c++17

#cgo !windows LDFLAGS: -L${SRCDIR}/../external/glslang -lglslang -lglslang-default-resource-limits -lSPIRV -lSPIRV-Tools-opt -lSPIRV-Tools
#cgo !windows LDFLAGS: -lstdc++

#cgo windows LDFLAGS: -L${SRCDIR}/../external/glslang
#cgo windows LDFLAGS: -lglslang -lglslang-default-resource-limits -lMachineIndependent -lGenericCodeGen -lOSDependent -lSPIRV -lSPIRV-Tools-opt -lSPIRV-Tools

// Note: On macOS, you may see "ld: warning: ignoring duplicate libraries: '-lc++'"
// This is harmless - it occurs because CGo links multiple C++ source files.
// To suppress: export CGO_LDFLAGS="-Wl,-no_warn_duplicate_libraries"

#include <stdlib.h>
#include "glslang_wrapper.h"
*/
import "C"

import (
	"fmt"
	"unsafe"
)

func init() {
	// Initialize glslang
	C.glslang_initialize()
}

// compileGLSLWithGlslang compiles GLSL compute shader source to SPIRV binary.
// Uses the Khronos glslang reference compiler with SPIRV-Tools optimization.
func compileGLSLWithGlslang(source string) ([]byte, error) {
	cSource := C.CString(source)
	defer C.free(unsafe.Pointer(cSource))

	var spirvData *C.char
	var spirvSize C.int
	var errorMsg *C.char

	result := C.compile_glsl_to_spirv(cSource, &spirvData, &spirvSize, &errorMsg)

	if result != 0 {
		if errorMsg != nil {
			errStr := C.GoString(errorMsg)
			C.free(unsafe.Pointer(errorMsg))
			return nil, fmt.Errorf("GLSL compilation failed: %s", errStr)
		}
		return nil, fmt.Errorf("GLSL compilation failed: unknown error")
	}

	if spirvData == nil || spirvSize == 0 {
		return nil, fmt.Errorf("GLSL compilation produced no output")
	}

	// Copy SPIRV binary data to Go bytes
	spirvBytes := C.GoBytes(unsafe.Pointer(spirvData), spirvSize)
	C.free(unsafe.Pointer(spirvData))

	return spirvBytes, nil
}
