package mental

/*
#cgo CFLAGS: -I${SRCDIR}/..
#cgo darwin LDFLAGS: ${SRCDIR}/../build/libmental-static.a ${SRCDIR}/../external/glslang/libglslang.a ${SRCDIR}/../external/glslang/libglslang-default-resource-limits.a ${SRCDIR}/../external/glslang/libSPIRV.a ${SRCDIR}/../external/glslang/libGenericCodeGen.a ${SRCDIR}/../external/glslang/libMachineIndependent.a ${SRCDIR}/../external/glslang/libOSDependent.a ${SRCDIR}/../external/spirv-cross/libspirv-cross-core.a ${SRCDIR}/../external/spirv-cross/libspirv-cross-glsl.a ${SRCDIR}/../external/spirv-cross/libspirv-cross-hlsl.a ${SRCDIR}/../external/spirv-cross/libspirv-cross-msl.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-opt.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-link.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-diff.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-lint.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-reduce.a ${SRCDIR}/../external/glslang/libSPIRV-Tools.a -lc++ -framework Metal -framework Foundation -framework QuartzCore -framework AppKit -framework OpenCL
#cgo linux LDFLAGS: ${SRCDIR}/../build/libmental-static.a ${SRCDIR}/../external/glslang/libglslang.a ${SRCDIR}/../external/glslang/libglslang-default-resource-limits.a ${SRCDIR}/../external/glslang/libSPIRV.a ${SRCDIR}/../external/glslang/libGenericCodeGen.a ${SRCDIR}/../external/glslang/libMachineIndependent.a ${SRCDIR}/../external/glslang/libOSDependent.a ${SRCDIR}/../external/spirv-cross/libspirv-cross-core.a ${SRCDIR}/../external/spirv-cross/libspirv-cross-glsl.a ${SRCDIR}/../external/spirv-cross/libspirv-cross-hlsl.a ${SRCDIR}/../external/spirv-cross/libspirv-cross-msl.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-opt.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-link.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-diff.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-lint.a ${SRCDIR}/../external/glslang/libSPIRV-Tools-reduce.a ${SRCDIR}/../external/glslang/libSPIRV-Tools.a -lstdc++ -lpthread -lrt
#cgo windows LDFLAGS: ${SRCDIR}/../build/libmental-static.a -lws2_32

#include "mental.h"
#include "transpile.h"
#include <stdlib.h>

// Forward declaration for the Go callback.
extern void mentalGoAtexitCallback(void);
*/
import "C"
import "io/fs"

// embedFS combines ReadFileFS and ReadDirFS for tool binary extraction.
type embedFS interface {
	fs.ReadFileFS
	ReadDir(name string) ([]fs.DirEntry, error)
}

// libFS is an embedded filesystem for tool binaries (DXC, Naga).
// When building with -tags embed, this is populated via go:embed.
// In dev builds it falls through to an empty FS.
var libFS embedFS = emptyFS{}

type emptyFS struct{}

func (emptyFS) Open(name string) (fs.File, error)         { return nil, fs.ErrNotExist }
func (emptyFS) ReadFile(name string) ([]byte, error)       { return nil, fs.ErrNotExist }
func (emptyFS) ReadDir(name string) ([]fs.DirEntry, error) { return nil, fs.ErrNotExist }

// Ensure emptyFS satisfies the embedFS interface.
var _ embedFS = emptyFS{}
