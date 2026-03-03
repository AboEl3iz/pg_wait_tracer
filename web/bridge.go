package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"log"
	"net/http"
	"os/exec"
	"strconv"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

// Bridge connects a browser WebSocket to a remote pgwt-server over SSH.
// Requests/responses are matched by JSON "id" field for concurrency.
type Bridge struct {
	cmd     *exec.Cmd
	stdin   io.WriteCloser
	stdout  *bufio.Reader
	mu      sync.Mutex             // protects stdin writes
	pending map[int64]chan []byte   // id -> response channel
	pendMu  sync.Mutex
	dead    chan struct{}           // closed when SSH process dies
}

func NewBridge(host, serverPath, traceDir string) *Bridge {
	return &Bridge{
		cmd:     exec.Command("ssh", host, serverPath, traceDir),
		pending: make(map[int64]chan []byte),
		dead:    make(chan struct{}),
	}
}

func (b *Bridge) Start() error {
	var err error
	b.stdin, err = b.cmd.StdinPipe()
	if err != nil {
		return fmt.Errorf("stdin pipe: %w", err)
	}
	stdout, err := b.cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("stdout pipe: %w", err)
	}
	b.stdout = bufio.NewReaderSize(stdout, 1024*1024) // 1MB buffer for large responses
	b.cmd.Stderr = log.Writer()

	if err := b.cmd.Start(); err != nil {
		return fmt.Errorf("ssh start: %w", err)
	}

	go b.readLoop()
	return nil
}

func (b *Bridge) Close() {
	if b.stdin != nil {
		b.stdin.Close()
	}
	if b.cmd.Process != nil {
		b.cmd.Process.Kill()
	}
}

// readLoop reads JSON lines from SSH stdout and dispatches to pending channels.
func (b *Bridge) readLoop() {
	defer close(b.dead)
	for {
		line, err := b.stdout.ReadBytes('\n')
		if err != nil {
			log.Printf("SSH stdout closed: %v", err)
			break
		}

		id := extractID(line)
		b.pendMu.Lock()
		ch, ok := b.pending[id]
		b.pendMu.Unlock()

		if ok {
			ch <- line
		}
	}

	// SSH died: close all pending
	b.pendMu.Lock()
	for _, ch := range b.pending {
		close(ch)
	}
	b.pending = make(map[int64]chan []byte)
	b.pendMu.Unlock()
}

// HandleWebSocket bridges a browser WebSocket to the SSH connection.
func (b *Bridge) HandleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("ws upgrade: %v", err)
		return
	}
	defer conn.Close()
	log.Println("Browser connected via WebSocket")

	var wsMu sync.Mutex // protects conn.WriteMessage

	for {
		_, msg, err := conn.ReadMessage()
		if err != nil {
			break
		}

		id := extractID(msg)

		// Register pending response
		ch := make(chan []byte, 1)
		b.pendMu.Lock()
		b.pending[id] = ch
		b.pendMu.Unlock()

		// Send to SSH stdin
		b.mu.Lock()
		fmt.Fprintf(b.stdin, "%s\n", msg)
		b.mu.Unlock()

		// Handle response asynchronously so we don't block the read loop
		go func(id int64, ch chan []byte) {
			var resp []byte
			select {
			case r, ok := <-ch:
				if ok {
					resp = r
				} else {
					resp = []byte(`{"id":` + strconv.FormatInt(id, 10) + `,"error":"connection lost"}`)
				}
			case <-time.After(30 * time.Second):
				resp = []byte(`{"id":` + strconv.FormatInt(id, 10) + `,"error":"timeout"}`)
			case <-b.dead:
				resp = []byte(`{"id":` + strconv.FormatInt(id, 10) + `,"error":"server disconnected"}`)
			}

			b.pendMu.Lock()
			delete(b.pending, id)
			b.pendMu.Unlock()

			wsMu.Lock()
			conn.WriteMessage(websocket.TextMessage, resp)
			wsMu.Unlock()
		}(id, ch)
	}
}

// extractID finds "id":N in a JSON byte slice.
func extractID(data []byte) int64 {
	key := []byte(`"id":`)
	idx := bytes.Index(data, key)
	if idx < 0 {
		return 0
	}
	p := data[idx+len(key):]
	// Skip whitespace
	for len(p) > 0 && (p[0] == ' ' || p[0] == '\t') {
		p = p[1:]
	}
	// Parse integer
	var n int64
	neg := false
	if len(p) > 0 && p[0] == '-' {
		neg = true
		p = p[1:]
	}
	for len(p) > 0 && p[0] >= '0' && p[0] <= '9' {
		n = n*10 + int64(p[0]-'0')
		p = p[1:]
	}
	if neg {
		n = -n
	}
	return n
}
