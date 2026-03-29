package mental

/*
#include "mental.h"
#include <stdlib.h>
*/
import "C"
import (
	"fmt"
	"strings"
	"unsafe"
)

// LibrarySource describes where a library or tool was loaded from.
type LibrarySource int

const (
	SourceBuiltin  LibrarySource = iota // statically linked (glslang, spirv-cross, Metal, etc.)
	SourceSystem                        // found in $PATH
	SourceEmbedded                      // extracted from embedded binary
	SourceNone                          // not available
)

func (s LibrarySource) String() string {
	switch s {
	case SourceBuiltin:
		return "builtin"
	case SourceSystem:
		return "system"
	case SourceEmbedded:
		return "embedded"
	default:
		return "none"
	}
}

// Library describes a runtime dependency (backend, transpiler, or tool).
type Library struct {
	Name      string
	Version   string // empty if unknown
	Available bool
	Source    LibrarySource
}

// State is a snapshot of mental's current runtime state.
// Returned by [GetState], which blocks until backend discovery is complete.
type State struct {
	// ActiveBackend is the API type selected by the fallback chain.
	ActiveBackend APIType

	// ActiveBackendName is the human-readable name (e.g. "Metal", "Vulkan").
	ActiveBackendName string

	// Devices is the list of available GPU devices.
	Devices []Device

	// Libraries is the list of all registered runtime dependencies.
	// Backends, transpilers, and tools register themselves dynamically.
	Libraries []Library
}

// GetState returns a snapshot of mental's current runtime state.
// Blocks until backend discovery is complete — the returned state
// reflects reality, not a guess.
func GetState() *State {
	ensureTools()
	cs := C.mental_state_get()
	if cs == nil {
		return &State{ActiveBackendName: "none"}
	}
	defer C.mental_state_free(cs)

	s := &State{
		ActiveBackend:     APIType(cs.active_backend),
		ActiveBackendName: C.GoString(cs.active_backend_name),
	}

	// Copy devices
	if cs.device_count > 0 && cs.devices != nil {
		devs := unsafe.Slice(cs.devices, int(cs.device_count))
		s.Devices = make([]Device, int(cs.device_count))
		for i := range s.Devices {
			s.Devices[i] = Device(unsafe.Pointer(devs[i]))
		}
	}

	// Copy libraries
	if cs.library_count > 0 && cs.libraries != nil {
		libs := unsafe.Slice(cs.libraries, int(cs.library_count))
		s.Libraries = make([]Library, int(cs.library_count))
		for i := range s.Libraries {
			name := C.GoString(libs[i].name)
			avail := libs[i].available != 0
			s.Libraries[i] = Library{
				Name:      name,
				Available: avail,
			}
			if libs[i].version != nil {
				s.Libraries[i].Version = C.GoString(libs[i].version)
			}
			// Determine source: tools tracked by configureTools(),
			// everything else is statically linked (builtin).
			if src, ok := toolSources[name]; ok {
				s.Libraries[i].Source = src
			} else if avail {
				s.Libraries[i].Source = SourceBuiltin
			} else {
				s.Libraries[i].Source = SourceNone
			}
		}
	}

	return s
}

// String returns a human-readable summary of the runtime state.
func (s *State) String() string {
	var b strings.Builder

	fmt.Fprintf(&b, "Mental State\n")
	fmt.Fprintf(&b, "  Backend:  %s\n", s.ActiveBackendName)

	if len(s.Devices) == 0 {
		fmt.Fprintf(&b, "  Devices:  (none)\n")
	} else {
		fmt.Fprintf(&b, "  Devices:\n")
		for i, d := range s.Devices {
			fmt.Fprintf(&b, "    [%d] %s (%s)\n", i, d.Name(), d.APIName())
		}
	}

	if len(s.Libraries) > 0 {
		fmt.Fprintf(&b, "  Libraries:\n")
		for _, lib := range s.Libraries {
			status := "x"
			if lib.Available {
				status = "✓"
			}
			ver := ""
			if lib.Version != "" {
				ver = " " + lib.Version
			}
			src := ""
			if lib.Available {
				src = " (" + lib.Source.String() + ")"
			}
			fmt.Fprintf(&b, "    [%s] %s%s%s\n", status, lib.Name, ver, src)
		}
	}

	return b.String()
}
