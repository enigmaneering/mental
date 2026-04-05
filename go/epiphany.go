package mental

import (
	"sync"
)

// Epiphany is the base interface for all events a process can experience.
// Every inbound message, link request, spark detection, and observation
// flows through this unified type.
type Epiphany interface {
	epiphany() // marker method -- prevents external implementation
}

// Inception is the first event every process receives.
// If Sparked is true, this process was created by another and
// the ParentLink is the connection to the parent.
// If Sparked is false, this is a root process.
type EpiphanyInception struct {
	Sparked    bool
	ParentLink *SparkLink[[]byte] // nil if not sparked
}

func (EpiphanyInception) epiphany() {}

// LinkEstablished is received when another process connects to us.
type EpiphanyLinkEstablished struct {
	Link *SparkLink[[]byte]
}

func (EpiphanyLinkEstablished) epiphany() {}

// MessageReceived is received when data arrives on any link.
type EpiphanyMessageReceived struct {
	Link *SparkLink[[]byte]
	Data []byte
}

func (EpiphanyMessageReceived) epiphany() {}

// LinkBroken is received when a link's peer dies or disconnects.
type EpiphanyLinkBroken struct {
	Link *SparkLink[[]byte]
}

func (EpiphanyLinkBroken) epiphany() {}

// ObservationReceived is received when a remote manifest responds
// to an observation request we made.
type EpiphanyObservationReceived struct {
	Link *SparkLink[[]byte]
	Data []byte // raw response (VALUE payload, or nil if denied/not found)
}

func (EpiphanyObservationReceived) epiphany() {}

// On is the process's main event loop.  It handles:
//   - Detecting if this process was sparked (parent link)
//   - Accepting incoming link connections from other processes
//   - Reading messages from all active links
//   - Detecting link breakage (peer death)
//
// The callback receives every event as an Epiphany.  On blocks forever.
//
//	func main() {
//	    mental.On(func(e mental.Epiphany) {
//	        switch ev := e.(type) {
//	        case mental.EpiphanySparked:
//	            fmt.Println("sparked by parent")
//	        case mental.EpiphanyLinkEstablished:
//	            fmt.Println("new peer connected")
//	        case mental.EpiphanyMessageReceived:
//	            fmt.Printf("message: %s\n", ev.Data)
//	        case mental.EpiphanyLinkBroken:
//	            fmt.Println("peer disconnected")
//	        }
//	    })
//	}

// incepted ensures the inception event is only sent once per process.
var incepted sync.Once

func On(handler func(Epiphany)) {
	ch := make(chan Epiphany, 64)
	var wg sync.WaitGroup

	// Inception -- sent exactly once, ever, as the first event.
	incepted.Do(func() {
		link := Sparked[[]byte]()
		inception := EpiphanyInception{
			Sparked:    link != nil,
			ParentLink: link,
		}
		ch <- inception

		// If sparked, start reading from the parent link
		if link != nil {
			wg.Add(1)
			go readLink(&wg, link, ch)
		}
	})

	// Accept incoming connections in the background
	wg.Add(1)
	go func() {
		defer wg.Done()
		for {
			link, err := LinkAccept[[]byte]()
			if err != nil {
				return // listener shut down
			}
			ch <- EpiphanyLinkEstablished{Link: link}
			// Start reading from this new link
			wg.Add(1)
			go readLink(&wg, link, ch)
		}
	}()

	// Dispatch events to the handler
	for e := range ch {
		handler(e)
	}
}

// readLink reads messages from a link and pushes them to the channel.
// Observation responses (wire protocol types 0x81-0x83) are routed
// as ObservationReceived.  Everything else is MessageReceived.
func readLink(wg *sync.WaitGroup, link *SparkLink[[]byte], ch chan<- Epiphany) {
	defer wg.Done()
	for {
		data, err := link.Recv()
		if err != nil {
			ch <- EpiphanyLinkBroken{Link: link}
			return
		}

		// Check if this is a wire protocol response
		if len(data) > 0 {
			switch data[0] {
			case 0x81: // VALUE
				if len(data) >= 5 {
					size := uint32(data[1])<<24 | uint32(data[2])<<16 | uint32(data[3])<<8 | uint32(data[4])
					if len(data) >= int(5+size) {
						ch <- EpiphanyObservationReceived{Link: link, Data: data[5 : 5+size]}
						continue
					}
				}
			case 0x82, 0x83: // DENIED, NOT_FOUND
				ch <- EpiphanyObservationReceived{Link: link, Data: nil}
				continue
			}
		}

		ch <- EpiphanyMessageReceived{Link: link, Data: data}
	}
}
