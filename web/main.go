package main

import (
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
)

//go:embed static
var staticFiles embed.FS

func main() {
	port := flag.Int("port", 8384, "local HTTP port")
	traceDir := flag.String("trace-dir", "/var/lib/pgsql/18/data/pg_wait_tracer/", "remote trace directory")
	serverPath := flag.String("server-path", "pgwt-server", "remote pgwt-server binary path")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: pgwt [flags] user@host\n\n")
		fmt.Fprintf(os.Stderr, "Connects to a PostgreSQL server over SSH and opens\n")
		fmt.Fprintf(os.Stderr, "a wait event investigation UI in the browser.\n\n")
		fmt.Fprintf(os.Stderr, "Flags:\n")
		flag.PrintDefaults()
		fmt.Fprintf(os.Stderr, "\nExamples:\n")
		fmt.Fprintf(os.Stderr, "  pgwt root@db-server\n")
		fmt.Fprintf(os.Stderr, "  pgwt --trace-dir /tmp/traces root@db-server\n")
		fmt.Fprintf(os.Stderr, "  pgwt --server-path /root/pg_wait_tracer/pgwt-server root@db-server\n")
	}
	flag.Parse()

	if flag.NArg() < 1 {
		flag.Usage()
		os.Exit(1)
	}
	host := flag.Arg(0)

	// Spawn SSH → pgwt-server on remote host
	bridge := NewBridge(host, *serverPath, *traceDir)
	if err := bridge.Start(); err != nil {
		log.Fatalf("Failed to start: %v", err)
	}
	defer bridge.Close()
	log.Printf("SSH connected to %s", host)

	// WebSocket endpoint
	http.HandleFunc("/ws", bridge.HandleWebSocket)

	// Serve embedded static files
	sub, _ := fs.Sub(staticFiles, "static")
	http.Handle("/", http.FileServer(http.FS(sub)))

	addr := fmt.Sprintf("localhost:%d", *port)
	log.Printf("Open http://%s in your browser", addr)
	openBrowser("http://" + addr)

	// Graceful shutdown on Ctrl+C
	go func() {
		c := make(chan os.Signal, 1)
		signal.Notify(c, os.Interrupt)
		<-c
		log.Println("Shutting down...")
		bridge.Close()
		os.Exit(0)
	}()

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
