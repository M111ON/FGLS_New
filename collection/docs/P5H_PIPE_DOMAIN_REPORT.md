# P5H Pipe Domain - Feature Report
## Session June 11, 2026

---

## 1. Concept

P5H Pipe Domain = optional sync fabric for heterogeneous routing spines.

Analogy:
- Main level = normal FiboClock routing
- Every 12 ticks = pipe entry
- Inside pipe = 10-phase window for cross-pipe peek/borrow
- Tick 12 = exit pipe -> merge state -> return to main level
- Opt-in only; skip it when not needed

---

## 2. Geometry Foundation

| Property | Value |
|----------|-------|
| 5HexSakura topology | 1 outer pentagon + 5 hexagons + 1 inner pentagon |
| Outer pentagon vertices | 234, 306, 18, 90, 162 degrees |
| Inner pentagon rotation | +36 degrees |
| Hexagon connect rule | outer[N] -> inner[(N-2)%5] |
| Per-hexagon span | 108 degrees |

### Scale Discovery

| Space | /12 | Granularity |
|-------|-----|-------------|
| GEO_FULL = 20736 | 1728 | total flowers |
| FiboClock = 1440 | 120 | flowers per cycle |
| Tower/FLUSH = 144 | 12 | flowers per tower |
| Block = 48 | 4 | flowers per block |

---

## 3. Architecture

### Module: `include/p5h_ribcage.h`

- `P5H_TICK_SPAN = 12`
- `P5H_FLOWERS_FULL = 1728`
- `P5H_PHASE_SLOTS = 10`
- `P5HFlower`
- `P5HField`
- `p5h_field_init()`
- `p5h_field_tick()`
- `p5h_field_observe()`
- `p5h_field_peek()`
- `p5h_is_barrier()`
- `p5h_is_flower_start()`
- `p5h_flower_id()`
- `p5h_phase_at_tick()`
- `p5h_phase_to_vertex()`

### Module: `src/fibo_spine.h`

- `P5HHerringbone`
- `p5h_hbone_init()`
- `p5h_hbone_step()`
- `p5h_hbone_step_textured()`
- `p5h_hbone_peek()`

---

## 4. Flower / Phase Timeline

```text
Tick    1  2  3  4  5  6  7  8  9 10 11 12
Phase   0  1  2  3  4  5  6  7  8  9     BARRIER
Texture O  I  O  I  O  I  O  I  O  I
```

O = outer pentagon edge
I = inner pentagon edge

---

## 5. Test Results

### Demo 1: `demo_p5h_barrier.c`

- 3 processes at different speeds
- 96 ticks total
- 8/8 barriers converged

### Demo 2: `demo_p5h_fibo.c`

- 3 herringbone pairs
- Fib rates: (1,2), (3,5), (8,13)
- 8/8 barriers converged

---

## 6. Compile Verification

| Configuration | Result |
|---------------|--------|
| `-DP5H_ENABLE` | clean |
| without flag | clean |
| `-Wall -Wextra -Wpedantic` | clean |

---

## 7. Files

- `include/p5h_ribcage.h`
- `src/fibo_spine.h`
- `demo_p5h_barrier.c`
- `demo_p5h_fibo.c`
- `docs/P5H_PIPE_DOMAIN_REPORT.md`

---

## 8. Next Steps

- `FIBO_EV_BARRIER` in `geo_fibo_clock.h`
- progressive merge tree
- multiple-face pipe peek
- integrate with actual router
