package mental

import (
	"embed"
	"io/fs"
)

// Platform tool binaries (DXC, Naga) are embedded into every build.
// configureTools() in tools.go checks $PATH first, then falls back
// to extracting from this embedded filesystem to a temp directory.
//
// The directory layout is:
//
//	lib/
//	  darwin-arm64/
//	    dxc
//	    libdxcompiler.dylib
//	    naga
//	  linux-amd64/
//	    ...
//	  windows-amd64/
//	    ...

//go:embed all:lib
var libFS embed.FS

// embedFS combines ReadFileFS and ReadDirFS for tool binary extraction.
type embedFS interface {
	fs.ReadFileFS
	ReadDir(name string) ([]fs.DirEntry, error)
}
