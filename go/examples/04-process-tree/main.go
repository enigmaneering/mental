// Example 04: Process Tree
//
// Demonstrates recursive sparking — each process sparks its own child,
// forming a tree.  The parent sends a "depth" counter; each child
// decrements it and sparks until depth reaches 0.
//
//	root (depth=3)
//	  └── child (depth=2)
//	        └── grandchild (depth=1)
//	              └── leaf (depth=0)
//
// Run:
//
//	go build -o process-tree . && ./process-tree
package main

import (
	"encoding/binary"
	"fmt"
	"os"

	mental "git.enigmaneering.org/mental/go"
)

const maxDepth = 3

func main() {
	mental.On(func(e mental.Epiphany) {
		switch ev := e.(type) {
		case mental.EpiphanyInception:
			if !ev.Sparked {
				// Root route
				fmt.Printf("root:  starting tree (max depth = %d)\n", maxDepth)

				self, _ := os.Executable()
				link, err := mental.Spark[[]byte](self)
				if err != nil {
					fmt.Fprintf(os.Stderr, "spark failed: %v\n", err)
					os.Exit(1)
				}

				depthBuf := make([]byte, 4)
				binary.BigEndian.PutUint32(depthBuf, maxDepth)
				link.Send(depthBuf)

				for {
					msg, err := link.Recv()
					if err != nil || len(msg) == 0 {
						break
					}
					fmt.Printf("root:  received: %s\n", string(msg))
				}

				fmt.Println("root:  tree complete")
				os.Exit(0)
			}

			// Child route
			parentLink := ev.ParentLink

			depthBuf, err := parentLink.Recv()
			if err != nil || len(depthBuf) < 4 {
				os.Exit(1)
			}
			depth := binary.BigEndian.Uint32(depthBuf)

			name := depthName(depth)
			fmt.Printf("%s: alive at depth %d\n", name, depth)

			report := fmt.Sprintf("%s reporting (depth=%d, uuid=%s)",
				name, depth, mental.UUID()[:8])
			parentLink.Send([]byte(report))

			if depth > 0 {
				self, _ := os.Executable()
				childLink, err := mental.Spark[[]byte](self)
				if err != nil {
					fmt.Fprintf(os.Stderr, "%s: spark failed: %v\n", name, err)
					os.Exit(1)
				}

				newDepth := make([]byte, 4)
				binary.BigEndian.PutUint32(newDepth, depth-1)
				childLink.Send(newDepth)

				for {
					msg, err := childLink.Recv()
					if err != nil || len(msg) == 0 {
						break
					}
					parentLink.Send(msg)
				}
			}

			fmt.Printf("%s: done\n", name)
			os.Exit(0)

		default:
		}
	})
}

func depthName(depth uint32) string {
	switch depth {
	case 3:
		return "child"
	case 2:
		return "grandchild"
	case 1:
		return "great-grandchild"
	case 0:
		return "leaf"
	default:
		return fmt.Sprintf("depth-%d", depth)
	}
}
