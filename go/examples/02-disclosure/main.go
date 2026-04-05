// Example 02: Disclosure-Controlled Observation
//
// Demonstrates the disclosure system:
//   - Parent describes an OPEN thought and an EXCLUSIVE thought
//   - Child observes both through the epiphany event loop
//   - Open thought succeeds, exclusive thought requires credentials
//
// Run:
//
//	go build -o disclosure . && ./disclosure
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"time"
	"unsafe"

	mental "git.enigmaneering.org/mental/go"
)

func main() {
	step := 0
	var parentLink *mental.SparkLink[[]byte]

	mental.On(func(e mental.Epiphany) {
		switch ev := e.(type) {
		case mental.EpiphanyInception:
			if !ev.Sparked {
				// Parent route
				manifest := mental.CreateManifest("secrets", 0)

				publicVal := float32(42.0)
				mental.Describe(manifest, "public-value", &publicVal)
				fmt.Printf("parent: described public-value = %.1f (open)\n", publicVal)

				secretVal := float32(3.14159)
				cred := "magic-word"
				mental.Describe(manifest, "secret-value", &secretVal, cred)
				fmt.Printf("parent: described secret-value = %.5f (exclusive, credential=%q)\n", secretVal, cred)

				self, _ := os.Executable()
				link, err := mental.Spark[[]byte](self)
				if err != nil {
					fmt.Fprintf(os.Stderr, "spark failed: %v\n", err)
					os.Exit(1)
				}

				go mental.HandleObservations(manifest, link)

				time.Sleep(500 * time.Millisecond)
				fmt.Println("parent: done")
				os.Exit(0)
			}

			// Child route — send first observation request
			parentLink = ev.ParentLink
			time.Sleep(50 * time.Millisecond)

			fmt.Println("child:  observing public-value (no credential)...")
			sendObserve(parentLink, "public-value", "")

		case mental.EpiphanyObservationReceived:
			step++
			switch step {
			case 1:
				// Response to: public-value (no credential)
				if ev.Data != nil {
					v := *(*float32)(unsafe.Pointer(&ev.Data[0]))
					fmt.Printf("child:  public-value = %.1f ✓\n", v)
				} else {
					fmt.Println("child:  public-value: DENIED (unexpected!)")
				}
				fmt.Println("child:  observing secret-value (no credential)...")
				sendObserve(parentLink, "secret-value", "")

			case 2:
				// Response to: secret-value (no credential) — should be denied
				if ev.Data == nil {
					fmt.Println("child:  secret-value: DENIED ✓ (as expected)")
				} else {
					fmt.Println("child:  secret-value: got data (unexpected!)")
				}
				fmt.Println("child:  observing secret-value (wrong credential)...")
				sendObserve(parentLink, "secret-value", "wrong-word")

			case 3:
				// Response to: secret-value (wrong credential) — should be denied
				if ev.Data == nil {
					fmt.Println("child:  secret-value: DENIED ✓ (as expected)")
				} else {
					fmt.Println("child:  secret-value: got data (unexpected!)")
				}
				fmt.Println("child:  observing secret-value (correct credential)...")
				sendObserve(parentLink, "secret-value", "magic-word")

			case 4:
				// Response to: secret-value (correct credential) — should succeed
				if ev.Data != nil {
					v := *(*float32)(unsafe.Pointer(&ev.Data[0]))
					fmt.Printf("child:  secret-value = %.5f ✓\n", v)
				} else {
					fmt.Println("child:  secret-value: DENIED (unexpected!)")
				}
				fmt.Println("child:  done")
				os.Exit(0)
			}

		default:
		}
	})
}

func sendObserve(link *mental.SparkLink[[]byte], name, credential string) {
	nameBytes := []byte(name)
	credBytes := []byte(credential)

	msg := make([]byte, 1+2+len(nameBytes)+2+len(credBytes))
	msg[0] = 0x01
	binary.BigEndian.PutUint16(msg[1:3], uint16(len(nameBytes)))
	copy(msg[3:], nameBytes)
	offset := 3 + len(nameBytes)
	binary.BigEndian.PutUint16(msg[offset:offset+2], uint16(len(credBytes)))
	if len(credBytes) > 0 {
		copy(msg[offset+2:], credBytes)
	}

	link.Send(msg)
}
