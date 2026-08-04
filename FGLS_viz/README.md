# FGLS_viz — Visualization Tools

ไฟล์ GUI ทั้งหมดสำหรับ visualizing FGLS pipeline

## Files

| File | Description |
|------|-------------|
| `gui_prototype.html` | Dashboard หลัก — Adaptive Storage 4-tier visualization, entropy histogram, block allocation heatmap |
| `pipeline_visualizer.html` | Real-time weight access flow — Silk Screen grid, animated pipeline, resolution levels |
| `debug_monitor.html` | Silk Screen debug monitor — WebSocket real-time, heatmap, value distribution, access log |
| `fgls_gui.html` | encode/decode GUI — drag-drop, manual path, เรียก API จริง |

## Usage

### Static (open directly)
```bash
# เปิดใน browser ได้เลย
start gui_prototype.html
start pipeline_visualizer.html
start debug_monitor.html
```

### With Backend (encode/decode จริง)
```bash
# ต้องรัน server ก่อน
python fgls_gui_server.py
# แล้วเปิด browser ไป http://localhost:8080
```

## Architecture

```
gui_prototype.html
├── Adaptive Storage Visualization (4-tier)
├── Entropy Histogram (256 buckets)
├── Block Allocation Heatmap (144×144 = 20736 cells)
└── Container Format Stats

pipeline_visualizer.html
├── Tensor List (GGUF weights)
├── Pipeline Canvas (animated flow)
├── Silk Screen Grid (10 boxes × 6 dirs)
└── Timeline (access pattern, resolution, data flow)

debug_monitor.html
├── Silk Screen Active Cells (pulse animation)
├── Box Access Heatmap (last 1000 samples)
├── Weight Value Distribution
├── Access Log
└── Bottom: Throughput / Resolution Mix / Ring Buffer
```

## Data Source

- `kis_adaptive_export.c` (ใน FGLS_kis) — export JSON data สำหรับ visualization
- Backend server — encode/decode/bench จริง
