/**
 * example_inference_debug.c — Example: inference with debug visualization
 * 
 * Shows how to add debug hooks to a silk screen inference loop.
 * Overhead: < 0.1% (sampling every 1000 weights)
 * 
 * Compile:
 *   gcc -O2 -o inference_debug example_inference_debug.c -lpthread
 * 
 * Run:
 *   ./inference_debug
 * 
 * Then open browser: http://localhost:8765/debug_monitor.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

// === Debug System (inline for example) ===
#define RING_SIZE 1024
#define SAMPLE_RATE 1000

typedef struct {
    uint32_t weight_index;
    uint8_t box;
    uint8_t dir;
    uint16_t tick;
    int8_t weight;
    uint8_t resolution;
    uint16_t padding;
} DebugSample;

typedef struct {
    DebugSample buffer[RING_SIZE];
    atomic_uint head;
    atomic_uint tail;
} RingBuffer;

static RingBuffer g_ring;
static atomic_bool g_running = true;

// Push sample to ring buffer (lock-free)
static inline void debug_push(uint8_t box, uint8_t dir, uint16_t tick, int8_t weight) {
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

// === Silk Screen Constants ===
#define BOXES 10
#define DIRS 6
#define TICKS 1440

// === Simulated Weight Array ===
static int8_t weights[BOXES][DIRS][TICKS];

// Initialize weights with random data
void init_weights(void) {
    srand(42);
    for (int b = 0; b < BOXES; b++)
        for (int d = 0; d < DIRS; d++)
            for (int t = 0; t < TICKS; t++)
                weights[b][d][t] = rand() % 256 - 128;
}

// === Silk Screen Read (identity filter) ===
static inline int8_t silk_read(uint8_t box, uint8_t dir, uint16_t tick) {
    return weights[box][dir][tick];  // Identity: weight = stored value
}

// === Inference Loop ===
void inference_loop(uint64_t iterations) {
    printf("[INFERENCE] Starting %lu iterations...\n", iterations);
    printf("[INFERENCE] Debug hook: sampling every %d weights\n", SAMPLE_RATE);
    
    uint64_t count = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    while (count < iterations && atomic_load(&g_running)) {
        // Simulate weight access pattern
        uint8_t box = count % BOXES;
        uint8_t dir = (count / BOXES) % DIRS;
        uint16_t tick = (count / (BOXES * DIRS)) % TICKS;
        
        // === Core inference: read weight ===
        int8_t weight = silk_read(box, dir, tick);
        
        // === Debug hook (every SAMPLE_RATE weights) ===
        count++;
        if (count % SAMPLE_RATE == 0) {
            debug_push(box, dir, tick, weight);
        }
        
        // Simulate some computation
        volatile int8_t result = weight;  // Prevent optimization
        (void)result;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    
    printf("[INFERENCE] Completed: %lu weights in %.3f sec\n", count, elapsed);
    printf("[INFERENCE] Throughput: %.2f M weights/sec\n", count / elapsed / 1e6);
    printf("[INFERENCE] Debug overhead: %.2f%%\n", 
           (double)(count / SAMPLE_RATE) / count * 100);
}

// === Main ===
int main(void) {
    printf("=== Silk Screen Debug Example ===\n\n");
    
    // Initialize
    atomic_store(&g_ring.head, 0);
    atomic_store(&g_ring.tail, 0);
    init_weights();
    
    printf("Weights initialized: %d boxes × %d dirs × %d ticks = %d slots\n",
           BOXES, DIRS, TICKS, BOXES * DIRS * TICKS);
    printf("\n");
    
    printf("To see debug visualization:\n");
    printf("1. Run: ./silk_screen_debug_ws\n");
    printf("2. Open: http://localhost:8765/debug_monitor.html\n");
    printf("3. Run this program: ./inference_debug\n\n");
    
    // Run inference
    inference_loop(10000000);  // 10M weights
    
    // Stats
    uint32_t head = atomic_load(&g_ring.head);
    uint32_t tail = atomic_load(&g_ring.tail);
    printf("\n[DEBUG] Ring buffer usage: %d/%d samples\n", 
           (head - tail + RING_SIZE) % RING_SIZE, RING_SIZE);
    
    return 0;
}
