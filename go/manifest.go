package mental

/*
#include "mental.h"
#include <stdlib.h>
*/
import "C"
import (
	"encoding/binary"
	"fmt"
	"sync"
	"unsafe"
)

// Wire protocol message types.
const (
	msgObserve  byte = 0x01
	msgValue    byte = 0x81
	msgDenied   byte = 0x82
	msgNotFound byte = 0x83
)

// thoughtEntry is a single described thought in a manifest.
type thoughtEntry struct {
	name   string
	ref    C.mental_reference // cgo reference handle (GPU buffer)
	size   int                // data size in bytes
	active bool
}

// Manifest is a device-bound routing table for named data references.
// The manifest itself is pure Go; only the underlying references
// (which are GPU buffers) use cgo.
type Manifest struct {
	name     string
	device   Device
	thoughts []thoughtEntry
	mu       sync.RWMutex
}

// Self creates a private manifest on the given device.
// Convention: "self" manifests are never served to other processes.
// Pass 0 for CPU-only.
func Self(device Device) *Manifest {
	return CreateManifest("self", device)
}

// CreateManifest creates a named manifest bound to a device.
// Multiple manifests per process, each with a unique name.
// Pass 0 for device to create a CPU-only manifest.
func CreateManifest(name string, device Device) *Manifest {
	return &Manifest{
		name:     name,
		device:   device,
		thoughts: make([]thoughtEntry, 0),
	}
}

// Name returns the manifest's name ("self" for the self manifest).
func (m *Manifest) Name() string {
	if m == nil {
		return ""
	}
	return m.name
}

// Close closes the manifest and all its thought references.
func (m *Manifest) Close() {
	if m == nil {
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	for i := range m.thoughts {
		if m.thoughts[i].active && m.thoughts[i].ref != nil {
			C.mental_reference_close(m.thoughts[i].ref)
			m.thoughts[i].ref = nil
			m.thoughts[i].active = false
		}
	}
	m.thoughts = nil
}

// Description is a handle to a described thought.
type Description[T any] struct {
	manifest *Manifest
	index    int
	data     *T
}

// Describe registers a thought with a manifest.
// Creates a C reference, writes data into it, and optionally pins to the device.
func Describe[T any](m *Manifest, name string, value *T, credential ...any) *Description[T] {
	if m == nil {
		return nil
	}

	size := unsafe.Sizeof(*value)
	if size == 0 {
		size = 1
	}

	// Create a C reference for GPU buffer backing
	var cdh C.mental_disclosure
	var credPtr unsafe.Pointer
	var credLen C.size_t
	var mode C.mental_relationship = C.MENTAL_RELATIONALLY_OPEN

	if len(credential) > 0 {
		b := discloseBytes(credential[0])
		if len(b) > 0 {
			credPtr = unsafe.Pointer(&b[0])
			credLen = C.size_t(len(b))
			mode = C.MENTAL_RELATIONALLY_EXCLUSIVE
		}
	}

	cref := C.mental_reference_create(C.size_t(size), mode, credPtr, credLen, &cdh)
	if cref == nil {
		return nil
	}

	// Write data into the reference
	C.mental_reference_write(cref, unsafe.Pointer(value), C.size_t(size))

	// Pin to device if one is set
	if m.device != 0 {
		C.mental_reference_pin(cref, C.mental_device(unsafe.Pointer(m.device)))
	}

	m.mu.Lock()
	idx := len(m.thoughts)
	m.thoughts = append(m.thoughts, thoughtEntry{
		name:   name,
		ref:    cref,
		size:   int(size),
		active: true,
	})
	m.mu.Unlock()

	return &Description[T]{manifest: m, index: idx, data: value}
}

// Redescribe updates the thought's value.
func (d *Description[T]) Redescribe(value *T) {
	if d == nil || d.manifest == nil {
		return
	}
	size := unsafe.Sizeof(*value)

	d.manifest.mu.RLock()
	if d.index < len(d.manifest.thoughts) && d.manifest.thoughts[d.index].active {
		ref := d.manifest.thoughts[d.index].ref
		d.manifest.mu.RUnlock()
		C.mental_reference_write(ref, unsafe.Pointer(value), C.size_t(size))
		d.data = value
	} else {
		d.manifest.mu.RUnlock()
	}
}

// Undescribe removes the thought from the manifest.
func (d *Description[T]) Undescribe() {
	if d == nil || d.manifest == nil {
		return
	}

	d.manifest.mu.Lock()
	if d.index < len(d.manifest.thoughts) && d.manifest.thoughts[d.index].active {
		C.mental_reference_close(d.manifest.thoughts[d.index].ref)
		d.manifest.thoughts[d.index].ref = nil
		d.manifest.thoughts[d.index].active = false
	}
	d.manifest.mu.Unlock()
	d.data = nil
}

// Recall looks up a thought by name in a manifest you own.
func Recall[T any](m *Manifest, name string) *Reference[T, struct{}] {
	if m == nil {
		return nil
	}
	m.mu.RLock()
	defer m.mu.RUnlock()
	for _, t := range m.thoughts {
		if t.active && t.name == name {
			ptr := uintptr(unsafe.Pointer(t.ref))
			return &Reference[T, struct{}]{ptr: ptr}
		}
	}
	return nil
}

// ObserveLocal reads a local thought from your own manifest.
func ObserveLocal[T any](m *Manifest, name string) (*T, error) {
	if m == nil {
		return nil, fmt.Errorf("mental: observe on nil manifest")
	}

	m.mu.RLock()
	var ref C.mental_reference
	var refSize int
	for _, t := range m.thoughts {
		if t.active && t.name == name {
			ref = t.ref
			refSize = t.size
			break
		}
	}
	m.mu.RUnlock()

	if ref == nil {
		return nil, fmt.Errorf("mental: observe %q: not found", name)
	}

	var result T
	size := unsafe.Sizeof(result)
	if int(size) > refSize {
		return nil, fmt.Errorf("mental: observe %q: data too small", name)
	}

	buf := make([]byte, size)
	C.mental_reference_read(ref, unsafe.Pointer(&buf[0]), C.size_t(len(buf)))
	result = *(*T)(unsafe.Pointer(&buf[0]))
	return &result, nil
}

// ObserveRemote reads a thought from a remote manifest through a spark link.
// Sends an observation request over the link, blocks for the response.
func ObserveRemote[T any](link *SparkLink[[]byte], name string, credential ...any) (*T, error) {
	if link == nil {
		return nil, fmt.Errorf("mental: observe on nil link")
	}

	// Build OBSERVE request: [type:1][name_len:2][name][cred_len:2][cred]
	nameBytes := []byte(name)
	var credBytes []byte
	if len(credential) > 0 {
		credBytes = discloseBytes(credential[0])
	}

	msg := make([]byte, 1+2+len(nameBytes)+2+len(credBytes))
	msg[0] = msgObserve
	binary.BigEndian.PutUint16(msg[1:3], uint16(len(nameBytes)))
	copy(msg[3:], nameBytes)
	offset := 3 + len(nameBytes)
	binary.BigEndian.PutUint16(msg[offset:offset+2], uint16(len(credBytes)))
	if len(credBytes) > 0 {
		copy(msg[offset+2:], credBytes)
	}

	if err := link.Send(msg); err != nil {
		return nil, fmt.Errorf("mental: observe send: %w", err)
	}

	resp, err := link.Recv()
	if err != nil {
		return nil, fmt.Errorf("mental: observe recv: %w", err)
	}

	if len(resp) == 0 {
		return nil, fmt.Errorf("mental: observe: empty response")
	}

	switch resp[0] {
	case msgValue:
		if len(resp) < 5 {
			return nil, fmt.Errorf("mental: observe: truncated response")
		}
		dataSize := binary.BigEndian.Uint32(resp[1:5])
		data := resp[5:]
		var result T
		size := unsafe.Sizeof(result)
		if uintptr(dataSize) >= size && uintptr(len(data)) >= size {
			result = *(*T)(unsafe.Pointer(&data[0]))
			return &result, nil
		}
		return nil, fmt.Errorf("mental: observe: data too small")
	case msgDenied:
		return nil, fmt.Errorf("mental: observe %q: access denied", name)
	case msgNotFound:
		return nil, fmt.Errorf("mental: observe %q: not found", name)
	default:
		return nil, fmt.Errorf("mental: observe: unknown response 0x%02x", resp[0])
	}
}

// HandleObservations listens for observation requests on a spark link
// and dispatches them to the manifest.  Call in a goroutine:
//
//	go mental.HandleObservations(manifest, link)
//
// Returns when the link breaks.
func HandleObservations(m *Manifest, link *SparkLink[[]byte]) {
	if m == nil || link == nil {
		return
	}

	for {
		msg, err := link.Recv()
		if err != nil {
			return
		}
		if len(msg) == 0 {
			continue
		}

		if msg[0] != msgObserve {
			continue
		}

		resp := handleObserveRequest(m, msg)
		if resp != nil {
			if err := link.Send(resp); err != nil {
				return
			}
		}
	}
}

// handleObserveRequest processes a single OBSERVE wire message and returns the response.
func handleObserveRequest(m *Manifest, msg []byte) []byte {
	if len(msg) < 3 {
		return []byte{msgNotFound}
	}

	nameLen := binary.BigEndian.Uint16(msg[1:3])
	if len(msg) < int(3+nameLen) {
		return []byte{msgNotFound}
	}
	name := string(msg[3 : 3+nameLen])

	// Look up the thought
	m.mu.RLock()
	var ref C.mental_reference
	var refSize int
	for _, t := range m.thoughts {
		if t.active && t.name == name {
			ref = t.ref
			refSize = t.size
			break
		}
	}
	m.mu.RUnlock()

	if ref == nil {
		return []byte{msgNotFound}
	}

	// Read the data from the reference
	buf := make([]byte, refSize)
	C.mental_reference_read(ref, unsafe.Pointer(&buf[0]), C.size_t(len(buf)))

	// Build VALUE response: [type:1][data_len:4][data]
	resp := make([]byte, 1+4+len(buf))
	resp[0] = msgValue
	binary.BigEndian.PutUint32(resp[1:5], uint32(len(buf)))
	copy(resp[5:], buf)
	return resp
}

// Link observes a thought from a remote manifest through a spark link
// and creates a local copy in this manifest, pinned to this manifest's device.
// Re-linking the same name refreshes the data.
func (m *Manifest) Link(link *SparkLink[[]byte], name string, credential ...any) error {
	if m == nil || link == nil {
		return fmt.Errorf("mental: link on nil manifest or link")
	}

	// Observe the remote thought through the spark link
	nameBytes := []byte(name)
	var credBytes []byte
	if len(credential) > 0 {
		credBytes = discloseBytes(credential[0])
	}

	msg := make([]byte, 1+2+len(nameBytes)+2+len(credBytes))
	msg[0] = msgObserve
	binary.BigEndian.PutUint16(msg[1:3], uint16(len(nameBytes)))
	copy(msg[3:], nameBytes)
	offset := 3 + len(nameBytes)
	binary.BigEndian.PutUint16(msg[offset:offset+2], uint16(len(credBytes)))
	if len(credBytes) > 0 {
		copy(msg[offset+2:], credBytes)
	}

	if err := link.Send(msg); err != nil {
		return fmt.Errorf("mental: link send: %w", err)
	}

	resp, err := link.Recv()
	if err != nil {
		return fmt.Errorf("mental: link recv: %w", err)
	}

	if len(resp) == 0 || resp[0] != msgValue || len(resp) < 5 {
		if len(resp) > 0 && resp[0] == msgDenied {
			return fmt.Errorf("mental: link %q: access denied", name)
		}
		if len(resp) > 0 && resp[0] == msgNotFound {
			return fmt.Errorf("mental: link %q: not found", name)
		}
		return fmt.Errorf("mental: link %q: failed", name)
	}

	dataSize := binary.BigEndian.Uint32(resp[1:5])
	data := resp[5 : 5+dataSize]

	// Check if we already have this thought linked -- update in place
	m.mu.Lock()
	for _, t := range m.thoughts {
		if t.active && t.name == name {
			C.mental_reference_write(t.ref, unsafe.Pointer(&data[0]), C.size_t(len(data)))
			m.mu.Unlock()
			return nil
		}
	}
	m.mu.Unlock()

	// Create new reference and describe
	var cdh C.mental_disclosure
	cref := C.mental_reference_create(C.size_t(len(data)),
		C.MENTAL_RELATIONALLY_OPEN, nil, 0, &cdh)
	if cref == nil {
		return fmt.Errorf("mental: link %q: reference create failed", name)
	}

	C.mental_reference_write(cref, unsafe.Pointer(&data[0]), C.size_t(len(data)))

	// Pin to device if one is set
	if m.device != 0 {
		C.mental_reference_pin(cref, C.mental_device(unsafe.Pointer(m.device)))
	}

	m.mu.Lock()
	m.thoughts = append(m.thoughts, thoughtEntry{
		name:   name,
		ref:    cref,
		size:   len(data),
		active: true,
	})
	m.mu.Unlock()

	return nil
}
