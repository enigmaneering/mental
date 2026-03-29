package mental

import (
	"testing"
)

func TestSparkEdgeCases(t *testing.T) {
	skipIfNoLibrary(t)

	// Sparked returns nil when not sparked.
	link := Sparked[[]byte]()
	if link != nil {
		t.Fatal("Sparked() should return nil for a non-sparked process")
	}
}
