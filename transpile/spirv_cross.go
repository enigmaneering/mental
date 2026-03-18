package transpile

/*
#cgo CPPFLAGS: -I${SRCDIR}/../external/spirv-cross

#cgo !windows LDFLAGS: -L${SRCDIR}/../external/spirv-cross -lspirv-cross-core -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-msl

#cgo windows LDFLAGS: -L${SRCDIR}/../external/spirv-cross
#cgo windows LDFLAGS: -lspirv-cross-core -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-msl

#include <stdlib.h>
#include "spirv_cross_wrapper.h"
*/
import "C"

import (
	"fmt"
	"unsafe"

	"git.enigmaneering.org/mental/language"
)

// spirvToTarget transpiles SPIRV binary to the target language using SPIRV-Cross.
// Supports GLSL, HLSL, and MSL output with platform-specific optimizations.
func spirvToTarget(spirv []byte, target language.Language) (string, error) {
	var cTarget C.TargetLanguage
	switch target {
	case language.GLSL:
		cTarget = C.TARGET_GLSL
	case language.HLSL:
		cTarget = C.TARGET_HLSL
	case language.MSL:
		cTarget = C.TARGET_MSL
	default:
		return "", fmt.Errorf("unsupported target language: %v", target)
	}

	var outputSource *C.char
	var errorMsg *C.char

	result := C.transpile_spirv(
		(*C.char)(unsafe.Pointer(&spirv[0])),
		C.int(len(spirv)),
		cTarget,
		&outputSource,
		&errorMsg,
	)

	if result != 0 {
		if errorMsg != nil {
			errStr := C.GoString(errorMsg)
			C.free(unsafe.Pointer(errorMsg))
			return "", fmt.Errorf("SPIRV-Cross transpilation failed: %s", errStr)
		}
		return "", fmt.Errorf("SPIRV-Cross transpilation failed: unknown error")
	}

	if outputSource == nil {
		return "", fmt.Errorf("SPIRV-Cross transpilation produced no output")
	}

	source := C.GoString(outputSource)
	C.free(unsafe.Pointer(outputSource))

	return source, nil
}
