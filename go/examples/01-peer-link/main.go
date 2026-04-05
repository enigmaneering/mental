// Example 01: Peer-to-Peer Linking
//
// Demonstrates cross-process linking without a parent-child relationship.
// The parent passes its UUID to the child over the spark link, and the
// child connects BACK to the parent using mental.LinkConnect — establishing
// a second, independent link.
//
// Run:
//
//	go build -o peer-link . && ./peer-link
package main

import (
	"fmt"
	"os"
	"time"

	mental "git.enigmaneering.org/mental/go"
)

func main() {
	mental.On(func(e mental.Epiphany) {
		switch ev := e.(type) {
		case mental.EpiphanyInception:
			if !ev.Sparked {
				// Parent route
				fmt.Println("parent: starting")
				uuid := mental.UUID()
				fmt.Printf("parent: my UUID is %s\n", uuid)

				self, _ := os.Executable()
				sparkLink, err := mental.Spark[[]byte](self)
				if err != nil {
					fmt.Fprintf(os.Stderr, "spark failed: %v\n", err)
					os.Exit(1)
				}
				sparkLink.Send([]byte(uuid))
				fmt.Println("parent: sent UUID to child over spark link")

				// Accept the child's peer connection.
				peerLink, err := mental.LinkAccept[[]byte]()
				if err != nil {
					fmt.Fprintf(os.Stderr, "accept failed: %v\n", err)
					os.Exit(1)
				}
				fmt.Println("parent: accepted peer connection from child!")

				// Exchange a message over the PEER link (not the spark link).
				peerLink.Send([]byte("hello over peer link!"))
				reply, _ := peerLink.Recv()
				fmt.Printf("parent: received over peer link: %q\n", string(reply))

				time.Sleep(100 * time.Millisecond)
				fmt.Println("parent: done")
				os.Exit(0)
			}

			// Child route
			parentLink := ev.ParentLink

			uuidBytes, _ := parentLink.Recv()
			parentUUID := string(uuidBytes)
			fmt.Printf("child:  received parent UUID: %s\n", parentUUID)

			peerLink, err := mental.LinkConnect[[]byte](parentUUID)
			if err != nil {
				fmt.Fprintf(os.Stderr, "child: peer connect failed: %v\n", err)
				os.Exit(1)
			}
			fmt.Println("child:  connected to parent via peer link!")

			msg, _ := peerLink.Recv()
			fmt.Printf("child:  received over peer link: %q\n", string(msg))
			peerLink.Send([]byte("hello back over peer link!"))

			fmt.Println("child:  done")
			os.Exit(0)

		default:
		}
	})
}
