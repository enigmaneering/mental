package mental

import "testing"

func TestGetState(t *testing.T) {
	s := GetState()
	if s == nil {
		t.Fatal("GetState returned nil")
	}

	t.Logf("\n%s", s)

	if s.ActiveBackendName == "" {
		t.Error("ActiveBackendName is empty")
	}
	t.Logf("Active backend: %s", s.ActiveBackendName)

	if len(s.Devices) == 0 {
		t.Log("No devices found (PoCL may not be available)")
	}
	for i, d := range s.Devices {
		t.Logf("  Device %d: %s (%s)", i, d.Name(), d.APIName())
	}

	if len(s.Libraries) == 0 {
		t.Error("No libraries registered")
	}
	for _, lib := range s.Libraries {
		status := "unavailable"
		if lib.Available {
			status = "available"
		}
		ver := lib.Version
		if ver == "" {
			ver = "(no version)"
		}
		t.Logf("  Library: %-15s %s  [%s]", lib.Name, ver, status)
	}
}
