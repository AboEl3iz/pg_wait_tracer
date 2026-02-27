package main

import (
	"bufio"
	"embed"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"sync"

	"github.com/gorilla/websocket"
)

//go:embed static
var staticFiles embed.FS

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintf(os.Stderr, "Usage: pgwt-poc <user@host> <remote-path>\n")
		fmt.Fprintf(os.Stderr, "Example: pgwt-poc root@db-server /root/pgwt-server-poc\n")
		os.Exit(1)
	}

	host := os.Args[1]
	remoteBin := os.Args[2]

	// Spawn ssh — inherits user's ~/.ssh/config, agent, keys
	cmd := exec.Command("ssh", host, remoteBin)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		log.Fatalf("stdin pipe: %v", err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		log.Fatalf("stdout pipe: %v", err)
	}
	cmd.Stderr = os.Stderr

	if err := cmd.Start(); err != nil {
		log.Fatalf("ssh start: %v", err)
	}
	log.Printf("SSH connected to %s", host)

	reader := bufio.NewReader(stdout)
	var mu sync.Mutex

	// WebSocket handler: bridge browser <-> SSH stdin/stdout
	http.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			log.Printf("ws upgrade: %v", err)
			return
		}
		defer conn.Close()
		log.Println("Browser connected via WebSocket")

		for {
			_, msg, err := conn.ReadMessage()
			if err != nil {
				break
			}

			mu.Lock()
			fmt.Fprintf(stdin, "%s\n", msg)
			line, err := reader.ReadString('\n')
			mu.Unlock()

			if err != nil {
				log.Printf("ssh read error: %v", err)
				break
			}
			conn.WriteMessage(websocket.TextMessage, []byte(line))
		}
	})

	// Serve embedded static files
	sub, _ := fs.Sub(staticFiles, "static")
	http.Handle("/", http.FileServer(http.FS(sub)))

	addr := "localhost:8384"
	log.Printf("Open http://%s in your browser", addr)
	openBrowser("http://" + addr)

	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatal(err)
	}
}

func openBrowser(url string) {
	switch runtime.GOOS {
	case "darwin":
		exec.Command("open", url).Start()
	default:
		exec.Command("xdg-open", url).Start()
	}
}
