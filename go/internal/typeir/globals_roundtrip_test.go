package typeir_test

import (
	"testing"

	"github.com/vx-lang/vxc/internal/typeir"
)

func TestGlobalsRoundTrip(t *testing.T) {
	m := &typeir.TypeModule{
		Globals: []typeir.GlobalIR{
			{Name: "g_counter", Type: typeir.Int},
			{Name: "g_name", Type: typeir.String},
		},
	}
	data, err := typeir.SerializeTypeModule(m)
	if err != nil {
		t.Fatal(err)
	}
	m2, err := typeir.DeserializeTypeModule(data)
	if err != nil {
		t.Fatal(err)
	}
	if len(m2.Globals) != 2 {
		t.Fatalf("expected 2 globals, got %d", len(m2.Globals))
	}
	if m2.Globals[0].Name != "g_counter" || m2.Globals[1].Name != "g_name" {
		t.Errorf("globals mismatch: %+v", m2.Globals)
	}
}
