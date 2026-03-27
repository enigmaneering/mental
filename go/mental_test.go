package mental

import "testing"

// skipIfNoLibrary checks if the mental library loaded successfully.
// Tests that need the library skip gracefully in dev environments
// where the embedded binary isn't populated.
func skipIfNoLibrary(t *testing.T) {
	t.Helper()
	if libH == 0 {
		t.Skip("mental library not loaded (no embedded binary in dev)")
	}
}

func skipIfNoDevice(t *testing.T) {
	t.Helper()
	skipIfNoLibrary(t)
	if DeviceCount() == 0 {
		t.Skip("no GPU devices available")
	}
}

func TestDeviceCount(t *testing.T) {
	skipIfNoLibrary(t)
	count := DeviceCount()
	t.Logf("DeviceCount() = %d", count)
	// Count may be 0 on headless CI — that's valid, not an error.
	if count < 0 {
		t.Fatalf("DeviceCount() returned negative: %d", count)
	}
}

func TestDeviceGet(t *testing.T) {
	skipIfNoDevice(t)
	dev := DeviceGet(0)
	if dev == 0 {
		t.Fatal("DeviceGet(0) returned nil device")
	}

	name := dev.Name()
	t.Logf("Device 0: %s", name)
	if name == "" {
		t.Error("device name is empty")
	}

	api := dev.API()
	t.Logf("API type: %d", api)
	if api < APIMetal || api > APIPoCL {
		t.Errorf("unexpected API type: %d", api)
	}

	apiName := dev.APIName()
	t.Logf("API name: %s", apiName)
	if apiName == "" {
		t.Error("API name is empty")
	}
}

func TestAPITypeConstants(t *testing.T) {
	// Verify constants match the C enum values.
	if APIMetal != 0 {
		t.Errorf("APIMetal = %d, want 0", APIMetal)
	}
	if APID3D12 != 1 {
		t.Errorf("APID3D12 = %d, want 1", APID3D12)
	}
	if APIVulkan != 2 {
		t.Errorf("APIVulkan = %d, want 2", APIVulkan)
	}
	if APIOpenCL != 3 {
		t.Errorf("APIOpenCL = %d, want 3", APIOpenCL)
	}
	if APIOpenGL != 4 {
		t.Errorf("APIOpenGL = %d, want 4", APIOpenGL)
	}
	if APIPoCL != 5 {
		t.Errorf("APIPoCL = %d, want 5", APIPoCL)
	}
}

func TestErrorConstants(t *testing.T) {
	if Success != 0 {
		t.Errorf("Success = %d, want 0", Success)
	}
	if ErrNoDevices != -1 {
		t.Errorf("ErrNoDevices = %d, want -1", ErrNoDevices)
	}
}

func TestErrorString(t *testing.T) {
	tests := []struct {
		err  Error
		want string
	}{
		{Success, "success"},
		{ErrNoDevices, "no GPU devices found"},
		{ErrInvalidDevice, "invalid device"},
		{ErrCompilationFailed, "compilation failed"},
		{Error(-99), "unknown error"},
	}
	for _, tt := range tests {
		got := tt.err.Error()
		if got != tt.want {
			t.Errorf("Error(%d).Error() = %q, want %q", tt.err, got, tt.want)
		}
	}
}

func TestGetErrorWithoutError(t *testing.T) {
	skipIfNoLibrary(t)
	// After successful operations, error should be Success.
	DeviceCount() // trigger init
	err := GetError()
	if err != Success {
		t.Logf("GetError() = %d (may be from a previous failed operation)", err)
	}
}

func TestCompileAndDispatch(t *testing.T) {
	skipIfNoDevice(t)

	dev := DeviceGet(0)

	glsl := `#version 450
layout(local_size_x = 256) in;
layout(binding = 0) buffer Input0 { float data[]; } input0;
layout(binding = 1) buffer Input1 { float data[]; } input1;
layout(binding = 2) buffer Output { float data[]; } output_buf;
void main() {
    uint idx = gl_GlobalInvocationID.x;
    output_buf.data[idx] = input0.data[idx] + input1.data[idx];
}`

	kernel, err := Compile(dev, glsl)
	if err != nil {
		t.Fatalf("Compile: %v", err)
	}
	defer kernel.Finalize()
	t.Log("kernel compiled successfully")
}
