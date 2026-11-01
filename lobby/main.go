// Castlevania Fighter Online lobby server.
//
// A tiny in-memory room list: hosts announce themselves while waiting for an
// opponent, players list the rooms and connect to the host directly. The server
// never relays game traffic, so it fits comfortably in a free hosting tier.
package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"html/template"
	"log"
	"net"
	"net/http"
	"os"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

const roomTTL = 35 * time.Second // hosts heartbeat every 10s

type room struct {
	ID       string    `json:"id"`
	Name     string    `json:"name"`
	IP       string    `json:"ip"`
	Port     int       `json:"port"`
	UDPPort  int       `json:"udp_port"`
	Version  string    `json:"version"`
	Open     string    `json:"open"`
	AgeSec   int       `json:"age_sec"`
	token    string
	created  time.Time
	lastSeen time.Time
}

type lobby struct {
	mu    sync.Mutex
	rooms map[string]*room
	perIP map[string]int
}

var (
	lb       = &lobby{rooms: map[string]*room{}, perIP: map[string]int{}}
	nameRe   = regexp.MustCompile(`[^\p{L}\p{N} _.\-!?']`)
	maxRooms = 500
)

func randID(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return hex.EncodeToString(b)
}

func clientIP(r *http.Request) string {
	// Render / most PaaS put the real client first in X-Forwarded-For.
	if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
		ip := strings.TrimSpace(strings.Split(xff, ",")[0])
		if net.ParseIP(ip) != nil {
			return ip
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func (l *lobby) sweep() {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := time.Now()
	for id, rm := range l.rooms {
		if now.Sub(rm.lastSeen) > roomTTL {
			l.perIP[rm.IP]--
			delete(l.rooms, id)
		}
	}
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func handleIP(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, 200, map[string]string{"ip": clientIP(r)})
}

func handleRooms(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		lb.sweep()
		lb.mu.Lock()
		out := make([]room, 0, len(lb.rooms))
		now := time.Now()
		for _, rm := range lb.rooms {
			c := *rm
			c.AgeSec = int(now.Sub(rm.created).Seconds())
			out = append(out, c)
		}
		lb.mu.Unlock()
		sort.Slice(out, func(i, j int) bool { return out[i].AgeSec < out[j].AgeSec })
		writeJSON(w, 200, out)
	case http.MethodPost:
		var req struct {
			Name    string `json:"name"`
			Port    int    `json:"port"`
			UDPPort int    `json:"udp_port"`
			Version string `json:"version"`
		}
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&req); err != nil {
			http.Error(w, "bad request", 400)
			return
		}
		name := strings.TrimSpace(nameRe.ReplaceAllString(req.Name, ""))
		if r := []rune(name); len(r) > 20 {
			name = string(r[:20])
		}
		if name == "" {
			name = "Player"
		}
		if req.Port <= 0 || req.Port > 65535 {
			http.Error(w, "bad port", 400)
			return
		}
		ip := clientIP(r)
		lb.sweep()
		lb.mu.Lock()
		defer lb.mu.Unlock()
		if len(lb.rooms) >= maxRooms || lb.perIP[ip] >= 4 {
			http.Error(w, "too many rooms", 429)
			return
		}
		rm := &room{
			ID: randID(6), Name: name, IP: ip, Port: req.Port, UDPPort: req.UDPPort,
			Version: strings.TrimSpace(req.Version), token: randID(16),
			created: time.Now(), lastSeen: time.Now(),
		}
		if len(rm.Version) > 40 {
			rm.Version = rm.Version[:40]
		}
		lb.rooms[rm.ID] = rm
		lb.perIP[ip]++
		log.Printf("room %s opened by %q (%s:%d)", rm.ID, rm.Name, ip, rm.Port)
		writeJSON(w, 200, map[string]string{"id": rm.ID, "token": rm.token, "ip": ip})
	default:
		http.Error(w, "method not allowed", 405)
	}
}

// /api/rooms/{id}/heartbeat and /api/rooms/{id}/leave
func handleRoomAction(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", 405)
		return
	}
	parts := strings.Split(strings.Trim(strings.TrimPrefix(r.URL.Path, "/api/rooms/"), "/"), "/")
	if len(parts) != 2 {
		http.NotFound(w, r)
		return
	}
	var req struct {
		Token string `json:"token"`
	}
	json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&req)
	lb.mu.Lock()
	defer lb.mu.Unlock()
	rm, ok := lb.rooms[parts[0]]
	if !ok || rm.token != req.Token {
		http.Error(w, "room not found", 404)
		return
	}
	switch parts[1] {
	case "heartbeat":
		rm.lastSeen = time.Now()
		writeJSON(w, 200, map[string]string{"open": rm.Open})
	case "leave":
		lb.perIP[rm.IP]--
		delete(lb.rooms, rm.ID)
		log.Printf("room %s closed", rm.ID)
		writeJSON(w, 200, map[string]string{"ok": "1"})
	default:
		http.NotFound(w, r)
	}
}

var page = template.Must(template.New("p").Parse(`<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="refresh" content="10">
<title>CvF Online Lobby</title><style>body{font-family:system-ui,sans-serif;background:#140b12;color:#eee;max-width:640px;margin:2rem auto;padding:0 16px}
h1{color:#c9a24a;font-weight:600}td,th{padding:.4rem .8rem;text-align:left;border-bottom:1px solid #3a2833}.m{color:#999}</style></head>
<body><h1>Castlevania Fighter Online</h1><p class="m">Players waiting for a match. Join them from the game: Online &gt; Lobby.</p>
{{if .}}<table><tr><th>Player</th><th>Waiting</th></tr>{{range .}}<tr><td>{{.Name}}</td><td>{{.AgeSec}}s</td></tr>{{end}}</table>
{{else}}<p>Nobody is hosting right now.</p>{{end}}</body></html>`))

func handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	lb.sweep()
	lb.mu.Lock()
	out := []room{}
	now := time.Now()
	for _, rm := range lb.rooms {
		c := *rm
		c.AgeSec = int(now.Sub(rm.created).Seconds())
		out = append(out, c)
	}
	lb.mu.Unlock()
	sort.Slice(out, func(i, j int) bool { return out[i].AgeSec < out[j].AgeSec })
	page.Execute(w, out)
}

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/", handleIndex)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) { w.Write([]byte("ok")) })
	mux.HandleFunc("/api/ip", handleIP)
	mux.HandleFunc("/api/rooms", handleRooms)
	mux.HandleFunc("/api/rooms/", handleRoomAction)
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	go func() {
		for range time.Tick(10 * time.Second) {
			lb.sweep()
		}
	}()
	log.Printf("CvF lobby listening on :%s", port)
	log.Fatal(http.ListenAndServe(":"+port, mux))
}
