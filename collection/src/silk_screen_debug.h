/**
 * silk_screen_debug.h — Lightweight debug visualization for inference
 * 
 * Usage:
 *   silk_screen_debug_init();
 *   // In inference loop (every 1000 weights):
 *   silk_screen_debug_sample(box, dir, tick, weight);
 *   // Cleanup:
 *   silk_screen_debug_close();
 * 
 * Overhead: < 0.1% (sampling every 1000 weights)
 * Output: WebSocket to browser visualization
 */

#ifndef SILK_SCREEN_DEBUG_H
#define SILK_SCREEN_DEBUG_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

// === Configuration ===
#define DEBUG_RING_SIZE      1024    // Must be power of 2
#define DEBUG_SAMPLE_RATE    1000    // Sample every N weights
#define DEBUG_WS_PORT        8765    // WebSocket port

// === Data Structures ===

// Single debug sample (16 bytes — fits cache line)
typedef struct {
    uint32_t weight_index;      // Global weight counter
    uint8_t  box;               // Box ID (0-9)
    uint8_t  dir;               // Direction (0-5)
    uint16_t tick;              // Clock tick (0-1439)
    int8_t   weight;            // Actual weight value
    uint8_t  resolution;        // Base-2 level (0=64, 1=128, 2=256)
    uint16_t padding;           // Alignment
} DebugSample;

// Lock-free ring buffer (SPSC)
typedef struct {
    DebugSample buffer[DEBUG_RING_SIZE];
    atomic_uint head;           // Producer writes here
    atomic_uint tail;           // Consumer reads here
} DebugRingBuffer;

// Debug context
typedef struct {
    DebugRingBuffer ring;
    uint64_t total_weights;     // Total weights processed
    uint64_t total_samples;     // Total samples taken
    bool initialized;
    int ws_fd;                  // WebSocket file descriptor
} SilkScreenDebug;

// === API ===

/**
 * Initialize debug system
 * @param port WebSocket port (0 = use default)
 * @return 0 on success, -1 on error
 */
int silk_screen_debug_init(int port);

/**
 * Sample a weight (call every N weights in inference loop)
 * @param box Box ID (0-9)
 * @param dir Direction (0-5)
 * @param tick Clock tick (0-1439)
 * @param weight Weight value
 */
static inline void silk_screen_debug_sample(
    uint8_t box, 
    uint8_t dir, 
    uint16_t tick, 
    int8_t weight
) {
    extern SilkScreenDebug g_silk_debug;
    
    // Fast path: increment counter, skip if not sampling
    g_silk_debug.total_weights++;
    if (g_silk_debug.total_weights % DEBUG_SAMPLE_RATE != 0) {
        return;
    }
    
    // Sampling path: create sample
    uint32_t head = atomic_load_explicit(&g_silk_debug.ring.head, memory_order_relaxed);
    uint32_t next = (head + 1) % DEBUG_RING_SIZE;
    
    // Check if ring is full (drop sample if so)
    uint32_t tail = atomic_load_explicit(&g_silk_debug.ring.tail, memory_order_acquire);
    if (next == tail) {
        return;  // Ring full, drop sample
    }
    
    // Write sample
    DebugSample *s = &g_silk_debug.ring.buffer[head];
    s->weight_index = g_silk_debug.total_weights;
    s->box = box;
    s->dir = dir;
    s->tick = tick;
    s->weight = weight;
    s->resolution = (weight >= 64) ? 2 : (weight >= -64) ? 1 : 0;  // Simplified
    
    // Publish (release ensures data is visible before head advances)
    atomic_store_explicit(&g_silk_debug.ring.head, next, memory_order_release);
    g_silk_debug.total_samples++;
}

/**
 * Get debug statistics
 */
void silk_screen_debug_stats(
    uint64_t *total_weights,
    uint64_t *total_samples,
    float *overhead_percent
);

/**
 * Close debug system
 */
void silk_screen_debug_close(void);

/**
 * Get singleton instance
 */
SilkScreenDebug* silk_screen_debug_instance(void);

#endif // SILK_SCREEN_DEBUG_H
