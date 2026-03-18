// Package mental provides primitives for thought: the ability to pin and observe data.
//
// mental builds the foundational layer for conscious computation through Observable References -
// data that exists in a quantum-like superposition until observed. These references form the
// basic building blocks of thought, enabling parallel observation and mutation across temporal
// perspectives.
//
// While mental leverages GPU compute for parallel processing, it abstracts away hardware details
// to present a unified model of consciousness: thoughts as observable state transformations.
//
// # Core Concepts
//
// mental is built around three main primitives:
//
//   - Thought: Observable references with synchronized access (pinned thoughts)
//   - Kernels: State transformation programs (thought processes)
//   - Info: Computational substrate metadata (mental architecture)
//
// # Observable References: Pinning Thoughts
//
// The Reference type implements observable references - data that can be pinned, observed,
// and mutated. Each observation creates a temporal snapshot while mutations evolve the
// underlying state. This enables multiple observers to experience different temporal
// perspectives of the same thought.
//
//	thought := mental.Thought.Alloc(1024)
//
//	// Observe current state (read-only snapshot)
//	snapshot := thought.Observe()
//
//	// Mutate state (exclusive access, evolves the thought)
//	thought.Mutate(func(data []byte) {
//	    for i := range data {
//	        data[i] = byte(i)
//	    }
//	})
//
// # Thought Processes: State Transformations
//
// Kernels define state transformation programs. Write in any supported language (GLSL, HLSL, MSL)
// and mental automatically transpiles to the native format for the underlying computational substrate.
//
//	process, _ := mental.Program.Compile(`
//	    #version 450
//	    layout(local_size_x = 256) in;
//	    layout(std430, binding = 0) buffer Input { uint data[]; } input;
//	    layout(std430, binding = 1) buffer Output { uint data[]; } output;
//
//	    void main() {
//	        uint id = gl_GlobalInvocationID.x;
//	        output.data[id] = input.data[id] * 2;
//	    }
//	`)
//
// # Computational Substrates
//
// mental automatically discovers and initializes available computational substrates (GPUs).
// Query and target specific substrates as needed:
//
//	// List all available substrates
//	substrates := mental.List()
//	for _, s := range substrates {
//	    fmt.Printf("%s (%s)\n", s.Name, s.API)
//	}
//
//	// Get specific substrate info
//	substrate := mental.Get(0)
//
//	// Allocate on specific substrate
//	ref := mental.Thought.Alloc(1024, substrate)
//
// # Platform Support
//
// mental automatically uses the appropriate API for each platform:
//
//   - macOS: Metal (native)
//   - Linux: Vulkan
//   - Windows: Direct3D 12 (preferred) or Vulkan
//
// # Example: Complete Thought Pipeline
//
//	package main
//
//	import (
//	    "fmt"
//	    "git.enigmaneering.org/mental"
//	)
//
//	func main() {
//	    // Pin thoughts (automatically freed by GC)
//	    input := mental.Thought.Alloc(1024)
//	    output := mental.Thought.Alloc(1024)
//
//	    // Populate initial state
//	    input.Mutate(func(data []byte) {
//	        for i := range data {
//	            data[i] = byte(i)
//	        }
//	    })
//
//	    // Define thought process
//	    process, _ := mental.Program.Compile(`
//	        #version 450
//	        layout(local_size_x = 256) in;
//	        layout(std430, binding = 0) buffer Input { uint data[]; } input;
//	        layout(std430, binding = 1) buffer Output { uint data[]; } output;
//
//	        void main() {
//	            uint id = gl_GlobalInvocationID.x;
//	            output.data[id] = input.data[id] * 2;
//	        }
//	    `)
//
//	    // Execute transformation
//	    process.Dispatch([]*mental.Reference{input}, output, 256)
//
//	    // Observe result
//	    result := output.Observe()
//	    fmt.Printf("Result: %v\n", result[:10])
//	}
package mental

var Module = "mental"
