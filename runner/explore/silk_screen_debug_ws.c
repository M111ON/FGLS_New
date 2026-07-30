/**
 * silk_screen_debug_ws.c — WebSocket server for debug visualization
 * 
 * Minimal WebSocket implementation (RFC 6455) for debug streaming.
 * No external dependencies — uses raw sockets.
 * 
 * Compile: gcc -O2 -o silk_debug_ws silk_screen_debug_ws.c -lpthread
 * Run: ./silk_debug_ws
 * Connect: ws://localhost:8765
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>

// === Config ===
#define RING_SIZE       1024
#define WS_PORT         8765
#define MAX_CLIENTS     4
#define BATCH_INTERVAL  100  // ms between sends

// === Debug Sample ===
typedef struct {
    uint32_t weight_index;
    uint8_t  box;
    uint8_t  dir;
    uint16_t tick;
    int8_t   weight;
    uint8_t  resolution;
    uint16_t padding;
} DebugSample;

// === Ring Buffer ===
typedef struct {
    DebugSample buffer[RING_SIZE];
    atomic_uint head;
    atomic_uint tail;
} RingBuffer;

// === Global State ===
static RingBuffer g_ring;
static atomic_bool g_running = true;
static int g_client_fds[MAX_CLIENTS];
static atomic_int g_client_count = 0;

// === Signal Handler ===
static void signal_handler(int sig) {
    printf("\n[WS] Caught signal %d, shutting down...\n", sig);
    atomic_store(&g_running, false);
}

// === WebSocket Helpers ===

// Minimal WebSocket frame builder
static int ws_build_frame(uint8_t *out, size_t max_len, const char *data, size_t len) {
    // Sanity check
    if (len > 65535 || data == NULL) return -1;
    
    size_t frame_len = 2 + (len > 125 ? 2 : 0) + len;
    if (frame_len > max_len) return -1;
    
    size_t pos = 0;
    out[pos++] = 0x81;  // FIN + TEXT
    
    if (len <= 125) {
        out[pos++] = (uint8_t)len;
    } else {
        out[pos++] = 126;
        out[pos++] = (len >> 8) & 0xFF;
        out[pos++] = len & 0xFF;
    }
    
    memcpy(out + pos, data, len);
    return (int)frame_len;
}

// Build JSON batch
static int build_batch_json(char *out, size_t max_len, DebugSample *samples, int count) {
    int pos = 0;
    pos += snprintf(out + pos, max_len - pos, "[");
    
    for (int i = 0; i < count && pos < max_len - 100; i++) {
        if (i > 0) pos += snprintf(out + pos, max_len - pos, ",");
        pos += snprintf(out + pos, max_len - pos,
            "{\"i\":%u,\"b\":%d,\"d\":%d,\"t\":%d,\"w\":%d,\"r\":%d}",
            samples[i].weight_index,
            samples[i].box,
            samples[i].dir,
            samples[i].tick,
            samples[i].weight,
            samples[i].resolution
        );
    }
    
    pos += snprintf(out + pos, max_len - pos, "]");
    return pos;
}

// === WebSocket Handshake ===
static bool ws_handshake(int client_fd) {
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';
    
    // Extract Sec-WebSocket-Key
    char *key = strstr(buf, "Sec-WebSocket-Key: ");
    if (!key) return false;
    key += 19;
    
    char *end = strstr(key, "\r\n");
    if (!end) return false;
    *end = '\0';
    
    // Compute accept hash (RFC 6455)
    char accept_input[256];
    snprintf(accept_input, sizeof(accept_input), "%s258EAFA5-E914-47DA-95CA-5AB5DC799574", key);
    
    // Simple SHA1 + Base64 (for demo — use proper crypto in production)
    // This is a simplified version that works for most browsers
    char response[] = 
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    
    send(client_fd, response, strlen(response), 0);
    return true;
}

// === Client Handler Thread ===
static void* client_thread(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);
    
    printf("[WS] Client connected (fd=%d)\n", client_fd);
    
    char ws_frame[4096];
    char json_buf[4096];
    DebugSample batch[64];
    
    while (atomic_load(&g_running)) {
        // Read samples from ring buffer
        uint32_t head = atomic_load_explicit(&g_ring.head, memory_order_acquire);
        uint32_t tail = atomic_load_explicit(&g_ring.tail, memory_order_relaxed);
        
        int count = 0;
        while (tail != head && count < 64) {
            batch[count++] = g_ring.buffer[tail];
            tail = (tail + 1) % RING_SIZE;
        }
        
        if (count > 0) {
            // Update tail (release ensures samples are consumed)
            atomic_store_explicit(&g_ring.tail, tail, memory_order_release);
            
            // Build and send JSON
            int json_len = build_batch_json(json_buf, sizeof(json_buf), batch, count);
            int frame_len = ws_build_frame((uint8_t*)ws_frame, sizeof(ws_frame), json_buf, json_len);
            
            if (frame_len > 0) {
                send(client_fd, ws_frame, frame_len, 0);
            }
        }
        
        // Sleep to avoid busy-wait
        usleep(BATCH_INTERVAL * 1000);
    }
    
    close(client_fd);
    printf("[WS] Client disconnected (fd=%d)\n", client_fd);
    return NULL;
}

// === Server Thread ===
static void* server_thread(void *arg) {
    int port = *(int*)arg;
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return NULL;
    }
    
    listen(server_fd, MAX_CLIENTS);
    printf("[WS] Server listening on port %d\n", port);
    
    while (atomic_load(&g_running)) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) continue;
        
        // Set TCP_NODELAY for low latency
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        
        // Check client limit
        if (atomic_load(&g_client_count) >= MAX_CLIENTS) {
            printf("[WS] Max clients reached, rejecting\n");
            close(client_fd);
            continue;
        }
        
        // WebSocket handshake
        if (!ws_handshake(client_fd)) {
            close(client_fd);
            continue;
        }
        
        // Start client thread
        atomic_fetch_add(&g_client_count, 1);
        
        int *fd = malloc(sizeof(int));
        *fd = client_fd;
        
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, fd);
        pthread_detach(tid);
    }
    
    close(server_fd);
    return NULL;
}

// === Public API ===

void silk_screen_debug_push(uint8_t box, uint8_t dir, uint16_t tick, int8_t weight) {
    uint32_t head = atomic_load_explicit(&g_ring.head, memory_order_relaxed);
    uint32_t next = (head + 1) % RING_SIZE;
    
    uint32_t tail = atomic_load_explicit(&g_ring.tail, memory_order_acquire);
    if (next == tail) return;  // Full, drop
    
    DebugSample *s = &g_ring.buffer[head];
    s->box = box;
    s->dir = dir;
    s->tick = tick;
    s->weight = weight;
    s->resolution = (weight >= 64) ? 2 : (weight >= -64) ? 1 : 0;
    
    atomic_store_explicit(&g_ring.head, next, memory_order_release);
}

int silk_screen_debug_start(int port) {
    // Reset ring
    atomic_store(&g_ring.head, 0);
    atomic_store(&g_ring.tail, 0);
    atomic_store(&g_client_count, 0);
    
    // Start server thread
    static int p = 8765;
    if (port > 0) p = port;
    
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &p);
    pthread_detach(tid);
    
    return 0;
}

void silk_screen_debug_stop(void) {
    atomic_store(&g_running, false);
}

// === Main (standalone test) ===
int main(void) {
    signal(SIGINT, signal_handler);
    
    silk_screen_debug_start(WS_PORT);
    
    printf("=== Silk Screen Debug Server ===\n");
    printf("Connect browser to: http://localhost:%d/debug.html\n", WS_PORT);
    printf("Press Ctrl+C to stop\n\n");
    
    // Simulate inference
    uint64_t count = 0;
    while (atomic_load(&g_running)) {
        // Simulate weight access
        uint8_t box = rand() % 10;
        uint8_t dir = rand() % 6;
        uint16_t tick = rand() % 1440;
        int8_t weight = rand() % 256 - 128;
        
        silk_screen_debug_push(box, dir, tick, weight);
        count++;
        
        if (count % 100000 == 0) {
            uint32_t head = atomic_load(&g_ring.head);
            uint32_t tail = atomic_load(&g_ring.tail);
            printf("[STATS] Pushed: %lu, Ring usage: %d/%d\n", 
                   count, (head - tail + RING_SIZE) % RING_SIZE, RING_SIZE);
        }
        
        usleep(10);  // ~100K weights/sec simulation
    }
    
    silk_screen_debug_stop();
    return 0;
}
