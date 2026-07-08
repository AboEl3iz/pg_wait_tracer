package main

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
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

	// UI-12: per-session WebSocket token.
	//
	// Threat model: this WebSocket can escalate a PRODUCTION daemon to
	// full-fidelity capture, so it must not be reachable by arbitrary web
	// pages. The localhost Origin check blocks pages served from other
	// origins (including DNS-rebinding pages, whose Origin stays the
	// attacker's hostname), but Origin is absent for non-browser clients, so
	// alone it is not enough. The token is a per-process random capability:
	// it is served only at /session on this localhost server and the page we
	// serve fetches it same-origin — a foreign page cannot read that response.
	// LIMIT: any process that can already reach localhost:PORT as this user
	// can fetch the token too; this is a same-user capability, not
	// authentication across local users or a substitute for SSH security.
	token := newSessionToken()

	// Spawn SSH → pgwt-server on remote host (supervised: respawns with
	// backoff if the SSH session or the server dies — UI-3).
	bridge := NewSSHBridge(host, *serverPath, *traceDir, token)
	if err := bridge.Start(); err != nil {
		log.Fatalf("Failed to start: %v", err)
	}
	defer bridge.Close()
	log.Printf("SSH connected to %s", host)

	// WebSocket endpoint (token-gated).
	http.HandleFunc("/ws", bridge.HandleWebSocket)

	// Session token endpoint: same-origin readable only (no CORS headers).
	http.HandleFunc("/session", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(map[string]string{"token": token})
	})

	// Serve embedded static files.
	sub, _ := fs.Sub(staticFiles, "static")
	http.Handle("/", http.FileServer(http.FS(sub)))

	addr := fmt.Sprintf("localhost:%d", *port)
	log.Printf("Open http://%s in your browser", addr)
	openBrowser("http://" + addr)

	// Graceful shutdown on Ctrl+C.
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

func newSessionToken() string {
	buf := make([]byte, 16)
	if _, err := rand.Read(buf); err != nil {
		log.Fatalf("session token: %v", err)
	}
	return hex.EncodeToString(buf)
}

func openBrowser(url string) {
	switch runtime.GOOS {
	case "darwin":
		exec.Command("open", url).Start()
	default:
		exec.Command("xdg-open", url).Start()
	}
}
