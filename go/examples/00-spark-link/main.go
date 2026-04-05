// Example 00: Epiphany-Driven Process
//
// Demonstrates the mental.On event loop:
//   - Parent describes a thought, sparks a child, handles observations
//   - Child uses mental.On to receive all events through one handler
//   - Observation results arrive as EpiphanyObservationReceived events
//
// Run:
//
//	go build -o spark-link . && ./spark-link
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"time"
	"unsafe"

	mental "git.enigmaneering.org/mental/go"
)

type Vec3 struct {
	X, Y, Z float32
}

func main() {
	fmt.Println(mental.GetState())

	observations := 0

	var parentLink *mental.SparkLink[[]byte]

	mental.On(func(e mental.Epiphany) {
		switch ev := e.(type) {
		case mental.EpiphanyInception:
			if !ev.Sparked {
				// Follow the parental route

				var device mental.Device
				if mental.DeviceCount() > 0 {
					device = mental.DeviceGet(0)
					fmt.Printf("parent: manifest on %s\n", device.Name())
				}

				manifest := mental.CreateManifest("physics", device)
				velocity := Vec3{1.0, 2.0, 3.0}
				desc := mental.Describe(manifest, "velocity", &velocity)
				fmt.Printf("parent: described velocity = %+v\n", velocity)

				self, _ := os.Executable()
				link, err := mental.Spark[[]byte](self)
				if err != nil {
					fmt.Fprintf(os.Stderr, "spark failed: %v\n", err)
					os.Exit(1)
				}

				go mental.HandleObservations(manifest, link)

				time.Sleep(300 * time.Millisecond)

				velocity = Vec3{10.0, 20.0, 30.0}
				desc.Redescribe(&velocity)
				fmt.Printf("parent: redescribed velocity = %+v\n", velocity)

				time.Sleep(500 * time.Millisecond)
				fmt.Println("parent: done")
				os.Exit(0)
			}

			// Follow the child route

			parentLink = ev.ParentLink
			fmt.Println("child:  inception — sparked by parent")
			sendObserveRequest(parentLink, "velocity")

		case mental.EpiphanyObservationReceived:
			observations++
			if ev.Data != nil && len(ev.Data) >= 12 {
				v := *(*Vec3)(unsafe.Pointer(&ev.Data[0]))
				if observations == 1 {
					fmt.Printf("child:  observed velocity = %+v\n", v)
					go func() {
						time.Sleep(400 * time.Millisecond)
						sendObserveRequest(parentLink, "velocity")
					}()
				} else {
					fmt.Printf("child:  observed velocity = %+v (updated)\n", v)
					fmt.Println("child:  done")
					os.Exit(0)
				}
			} else {
				fmt.Println("child:  observation denied or not found")
			}

		case mental.EpiphanyLinkBroken:
			fmt.Println("child:  link broken")
			os.Exit(0)

		case mental.EpiphanyMessageReceived:
			fmt.Printf("child:  raw message (%d bytes)\n", len(ev.Data))

		default:
		}
	})
}

func sendObserveRequest(link *mental.SparkLink[[]byte], name string) {
	nameBytes := []byte(name)
	msg := make([]byte, 1+2+len(nameBytes)+2)
	msg[0] = 0x01 // OBSERVE
	binary.BigEndian.PutUint16(msg[1:3], uint16(len(nameBytes)))
	copy(msg[3:], nameBytes)
	binary.BigEndian.PutUint16(msg[3+len(nameBytes):], 0)
	link.Send(msg)
}
