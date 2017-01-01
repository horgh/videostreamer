package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/http/fcgi"
	"os"
	"unsafe"
)

// #include "videostreamer.h"
// #include <stdlib.h>
// #cgo LDFLAGS: -lavformat -lavdevice -lavcodec -lavutil
// #cgo CFLAGS: -std=c11
// #cgo pkg-config: libavcodec
import "C"

// Args holds command line arguments.
type Args struct {
	ListenHost  string
	ListenPort  int
	InputFormat string
	InputURL    string
	Verbose     bool
	// Serve with FCGI protocol (true) or HTTP (false).
	FCGI bool
}

// HTTPHandler allows us to pass information to our request handlers.
type HTTPHandler struct {
	Verbose          bool
	ClientChangeChan chan<- int
	ClientChan       chan<- Client
}

// A Client is servicing one HTTP client. It receives frames from the reader.
type Client struct {
	Data chan []byte
	Done chan struct{}
}

func main() {
	args, err := getArgs()
	if err != nil {
		log.Fatalf("Invalid argument: %s", err)
	}

	C.vs_setup()

	// Encoder writes to out pipe. Reader reads from in pipe.
	in, out, err := os.Pipe()
	if err != nil {
		log.Fatalf("pipe: %s", err)
	}

	// Changes in clients announce on this channel. +1 for new client, -1 for
	// losing a client.
	clientChangeChan := make(chan int)

	// Clients provide reader a channel to receive on.
	//
	// The reader acts as a publisher and clients act as subscribers. One
	// publisher, potentially many subscribers.
	clientChan := make(chan Client)

	// When encoder writes a frame, we know how large it is by a message on this
	// channel. The encoder sends messages on this channel to the reader to
	// inform it of this. The reader then knows how much to read and allows it to
	// always read a single frame at a time, which is valid for a client to
	// receive. Otherwise if it reads without knowing frame boundaries, it is
	// difficult for it to know when it is valid to start sending data to a
	// client that enters mid-encoding.
	frameChan := make(chan int, 128)

	go encoderSupervisor(out, args.InputFormat, args.InputURL, args.Verbose,
		clientChangeChan, frameChan)
	go reader(args.Verbose, in, clientChan, frameChan)

	// Start serving either with HTTP or FastCGI.

	hostPort := fmt.Sprintf("%s:%d", args.ListenHost, args.ListenPort)

	handler := HTTPHandler{
		Verbose:          args.Verbose,
		ClientChangeChan: clientChangeChan,
		ClientChan:       clientChan,
	}

	if args.FCGI {
		listener, err := net.Listen("tcp", hostPort)
		if err != nil {
			log.Fatalf("Unable to listen: %s", err)
		}

		log.Printf("Starting to serve requests on %s (FastCGI)", hostPort)

		err = fcgi.Serve(listener, handler)
		if err != nil {
			log.Fatalf("Unable to serve: %s", err)
		}
	} else {
		s := &http.Server{
			Addr:    hostPort,
			Handler: handler,
		}

		log.Printf("Starting to serve requests on %s (HTTP)", hostPort)

		err = s.ListenAndServe()
		if err != nil {
			log.Fatalf("Unable to serve: %s", err)
		}
	}
}

// getArgs retrieves and validates command line arguments.
func getArgs() (Args, error) {
	listenHost := flag.String("host", "0.0.0.0", "Host to listen on.")
	listenPort := flag.Int("port", 8080, "Port to listen on.")
	format := flag.String("format", "pulse", "Input format. Example: rtsp for RTSP.")
	input := flag.String("input", "", "Input URL valid for the given format. For RTSP you can provide a rtsp:// URL.")
	verbose := flag.Bool("verbose", false, "Enable verbose logging output.")
	fcgi := flag.Bool("fcgi", true, "Serve using FastCGI (true) or as a regular HTTP server.")

	flag.Parse()

	if len(*listenHost) == 0 {
		flag.PrintDefaults()
		return Args{}, fmt.Errorf("you must provide a host")
	}

	if len(*format) == 0 {
		flag.PrintDefaults()
		return Args{}, fmt.Errorf("you must provide an input format")
	}

	if len(*input) == 0 {
		flag.PrintDefaults()
		return Args{}, fmt.Errorf("you must provide an input URL")
	}

	return Args{
		ListenHost:  *listenHost,
		ListenPort:  *listenPort,
		InputFormat: *format,
		InputURL:    *input,
		Verbose:     *verbose,
		FCGI:        *fcgi,
	}, nil
}

// The encoder supervisor deals with stopping and starting the encoder.
//
// We want there to be at most a single encoder goroutine active at any one
// time no matter how many clients there are. If there are zero clients, there
// should not be any encoding going on.
func encoderSupervisor(outPipe *os.File, inputFormat, inputURL string,
	verbose bool, clientChangeChan <-chan int, frameChan chan<- int) {
	// A count of how many clients are actively subscribed listening for
	// audio/video. We start the encoder when this goes above zero, and stop it
	// if it goes to zero.
	clients := 0

	// We close this channel to tell the encoder to stop. It receives no values.
	var encoderStopChan chan struct{}

	// The encoder tells us when it stops by sending a message on this channel.
	// Note I use sending a message as if this channel closes then the below loop
	// will be busy until it is re-opened.
	encoderDoneChan := make(chan struct{})

	for {
		select {
		// A change in the number of clients.
		case change := <-clientChangeChan:
			// Gaining a client.
			if change == 1 {
				clients++

				if verbose {
					log.Printf("encoder supervisor: new client. %d clients connected",
						clients)
				}

				if clients == 1 {
					if verbose {
						log.Printf("encoder supervisor: starting encoder")
					}

					encoderStopChan = make(chan struct{})

					go videoEncoder(outPipe, inputFormat, inputURL, encoderStopChan,
						encoderDoneChan, frameChan)
				}

				continue
			}

			// Losing a client.
			clients--

			if verbose {
				log.Printf("encoder supervisor: lost client. %d clients connected",
					clients)
			}

			if clients == 0 {
				// Tell encoder to stop.
				close(encoderStopChan)
				if verbose {
					log.Printf("encoder supervisor: stopping encoder")
				}
				continue
			}

		// Encoder stopped for some reason. Restart it if appropriate.
		case <-encoderDoneChan:
			if verbose {
				log.Printf("encoder supervisor: encoder stopped")
			}

			if clients == 0 {
				// No clients. We don't need to restart it.
				continue
			}

			if verbose {
				log.Printf("encoder supervisor: starting encoder")
			}

			encoderStopChan = make(chan struct{})

			go videoEncoder(outPipe, inputFormat, inputURL, encoderStopChan,
				encoderDoneChan, frameChan)
		}
	}
}

// videoEncoder opens a video input and begins decoding.
//
// It remuxes the video to an mp4 container.
//
// As it writes out frames, it informs the reader about a frame to read, along
// with that frame's size.
func videoEncoder(outPipe *os.File, inputFormat, inputURL string,
	stopChan <-chan struct{}, doneChan chan<- struct{}, frameChan chan<- int) {
	inputFormatC := C.CString(inputFormat)
	inputURLC := C.CString(inputURL)
	verbose := C.bool(true)

	input := C.vs_open_input(inputFormatC, inputURLC, verbose)
	if input == nil {
		log.Printf("Unable to open input")
		C.free(unsafe.Pointer(inputFormatC))
		C.free(unsafe.Pointer(inputURLC))
		doneChan <- struct{}{}
		return
	}
	C.free(unsafe.Pointer(inputFormatC))
	C.free(unsafe.Pointer(inputURLC))
	defer C.vs_destroy_input(input)

	outputFormatC := C.CString("mp4")
	outputURLC := C.CString(fmt.Sprintf("pipe:%d", outPipe.Fd()))

	output := C.vs_open_output(outputFormatC, outputURLC, input, verbose)
	if output == nil {
		log.Printf("Unable to open output")
		C.free(unsafe.Pointer(outputFormatC))
		C.free(unsafe.Pointer(outputURLC))
		doneChan <- struct{}{}
		return
	}
	C.free(unsafe.Pointer(outputFormatC))
	C.free(unsafe.Pointer(outputURLC))
	defer C.vs_destroy_output(output)

	for {
		select {
		// If stop channel is closed then we stop what we're doing.
		case <-stopChan:
			log.Printf("Stopping encoder")
			doneChan <- struct{}{}
			return
		default:
		}

		var pkt C.AVPacket
		readRes := C.int(0)
		readRes = C.vs_read_packet(input, &pkt, verbose)
		if readRes == -1 {
			log.Printf("Failure reading")
			doneChan <- struct{}{}
			return
		}

		if readRes == 0 {
			continue
		}

		writeRes := C.int(0)
		writeRes = C.vs_write_packet(input, output, &pkt, verbose)
		if writeRes == -1 {
			log.Printf("Failure writing")
			C.av_packet_unref(&pkt)
			doneChan <- struct{}{}
			return
		}

		pktSize := int(pkt.size)

		C.av_packet_unref(&pkt)

		frameChan <- int(pktSize)
	}
}

// reader reads the pipe containing the re-encoded audio/video.
//
// We send the re-encoded data to each client.
//
// We hear about new clients on the clients channel.
//
// We expect the pipe to never close. The encoder may stop sending for a while
// but when a new client appears, it starts again.
func reader(verbose bool, inPipe *os.File, clientChan <-chan Client,
	frameChan <-chan int) {
	reader := bufio.NewReader(inPipe)
	clients := []Client{}

	for {
		select {
		case client := <-clientChan:
			if verbose {
				log.Printf("reader: accepted new client")
			}

			clients = append(clients, client)

		case frameSize := <-frameChan:
			if verbose {
				log.Printf("reader: reading new frame (%d bytes)", frameSize)
			}

			frame, err := readFrame(reader, frameSize)
			if err != nil {
				log.Printf("reader: %s", err)
				return
			}

			if verbose {
				log.Printf("reader: read frame (%d bytes)", frameSize)
			}

			clients = sendFrameToClients(clients, frame)
		}
	}
}

// Read a frame.
func readFrame(reader *bufio.Reader, size int) ([]byte, error) {
	buf := []byte{}
	bytesNeeded := size

	for {
		if bytesNeeded == 0 {
			return buf, nil
		}

		readBuf := make([]byte, bytesNeeded)

		n, err := reader.Read(readBuf)
		if err != nil {
			return nil, fmt.Errorf("read: %s", err)
		}

		log.Printf("read %d bytes", n)

		buf = append(buf, readBuf[0:n]...)

		bytesNeeded -= n
	}
}

// Try to send the given frame to each client.
//
// If sending would block, cut the client off.
func sendFrameToClients(clients []Client, frame []byte) []Client {
	clients2 := []Client{}

	for _, client := range clients {
		err := sendFrameToClient(client, frame)
		if err != nil {
			continue
		}

		// Keep it around.
		clients2 = append(clients2, client)
	}

	return clients2
}

// Send a frame to a client. Don't block. If we would block, return an error.
// This lets us know if we should consider the client dead.
//
// Close the client's frame channel if we fail to send so it knows to go away.
func sendFrameToClient(client Client, frame []byte) error {
	select {
	case client.Data <- frame:
		return nil
	case <-client.Done:
		close(client.Data)
		return fmt.Errorf("client went away")
	default:
		close(client.Data)
		return fmt.Errorf("client is too slow")
	}
}

// ServeHTTP handles an HTTP request.
func (h HTTPHandler) ServeHTTP(rw http.ResponseWriter, r *http.Request) {
	log.Printf("Serving [%s] request from [%s] to path [%s] (%d bytes)",
		r.Method, r.RemoteAddr, r.URL.Path, r.ContentLength)

	if r.Method == "GET" && r.URL.Path == "/stream" {
		h.streamRequest(rw, r)
		return
	}

	log.Printf("Unknown request.")
	rw.WriteHeader(http.StatusNotFound)
	_, _ = rw.Write([]byte("<h1>404 Not found</h1>"))
}

func (h HTTPHandler) streamRequest(rw http.ResponseWriter, r *http.Request) {
	c := Client{
		// We receive audio/video data on this channel from the reader.
		Data: make(chan []byte, 1024),

		// We close this channel to indicate to reader we're done. This is necessary
		// if we terminate, otherwise the reader can't know to stop sending us
		// frames.
		Done: make(chan struct{}),
	}

	// Tell the reader we're here.
	h.ClientChan <- c

	// Tell the encoder we're here.
	h.ClientChangeChan <- 1

	rw.Header().Set("Content-Type", "video/mp4")

	rw.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")

	// We send chunked by default

	for {
		frame, ok := <-c.Data

		// Reader may have cut us off.
		if !ok {
			log.Printf("reader closed audio/video channel")
			break
		}

		n, err := rw.Write(frame)
		if err != nil {
			log.Printf("write: %s", err)
			break
		}

		if n != len(frame) {
			log.Printf("short write")
			break
		}

		// ResponseWriter buffers chunks. Flush them out ASAP to reduce the time a
		// client is waiting, especially initially.
		if flusher, ok := rw.(http.Flusher); ok {
			flusher.Flush()
		}

		if h.Verbose {
			//log.Printf("%s: Sent %d bytes to client", r.RemoteAddr, n)
		}
	}

	h.ClientChangeChan <- -1

	close(c.Done)

	// Drain frames channel.
	for range c.Data {
	}

	log.Printf("%s: Client cleaned up", r.RemoteAddr)
}
