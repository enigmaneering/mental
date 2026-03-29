// Example 00: Spark Link
//
// Demonstrates parent→child communication via mental.Spark.
//
// The parent process sparks a child, sends a greeting, and receives
// a response.  The child detects it was sparked, reads the greeting,
// and replies.  Both sides use the same binary — the child detects
// it was sparked via mental.Sparked.
//
// Run:
//
//	go build -o spark-link . && ./spark-link
//
// Note: requires libmental-static.a to be built (cmake --build build).
package main

import (
	"fmt"
	"os"

	mental "git.enigmaneering.org/mental/go"
)

func main() {
	if len(os.Args) > 1 {
		fmt.Println("Input arguments: " + os.Args[1])
	}

	fmt.Println(mental.GetState())

	// If we were sparked, run as the child.
	if link := mental.Sparked[string](); link != nil {
		child(link)
		return
	}

	// Otherwise, run as the parent.
	parent()
}

func parent() {
	// Spark a child using our own binary.
	self, err := os.Executable()
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to get executable path: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("parent: sparking child")
	link, err := mental.Spark[string](self + " \\\"Hello,\\ World!\\\"")
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to spark child: %v\n", err)
		os.Exit(1)
	}

	// Send a greeting to the child.
	fmt.Println("parent: sending greeting...")
	if err := link.Send("Hello from parent!"); err != nil {
		fmt.Fprintf(os.Stderr, "parent send failed: %v\n", err)
		os.Exit(1)
	}

	// Wait for the child's response.
	reply, err := link.Recv()
	if err != nil {
		fmt.Fprintf(os.Stderr, "parent recv failed: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("parent: received: %q\n", reply)
}

func child(link *mental.SparkLink[string]) {
	// Read the greeting from the parent.
	msg, err := link.Recv()
	if err != nil {
		fmt.Fprintf(os.Stderr, "child recv failed: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("child:  received: %q\n", msg)

	// Reply.
	if err := link.Send("Hello from child!"); err != nil {
		fmt.Fprintf(os.Stderr, "child send failed: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("child:  sent reply")
}
