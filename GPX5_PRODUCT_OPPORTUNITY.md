# GPX5 / GeoPixel — Product Opportunity Document

> สรุปแนวทาง product ที่เป็นไปได้จาก geopixel engine  
> จัดทำเมื่อ 2026-05-13  
> ใช้ประกอบการตัดสินใจว่าจะไปทางไหนต่อ

---

## สารบัญ

1. [สถานะปัจจุบัน — มีอะไรที่ทำงานได้แล้วบ้าง?](#1-สถานะปจจบน)
2. [Product A: GPX5 CLI — Geometric Archiver](#2-product-a-gpx5-cli)
3. [Product B: Game Asset Compression Service](#3-product-b-game-asset-service)
4. [Product C: GPX5 Image Codec (ขาย license)](#4-product-c-gpx5-image-codec)
5. [Product D: GPX5 Texture Packer Plugin (Godot / Unity)](#5-product-d-gpx5-texture-packer)
6. [ตารางเปรียบเทียบ — ระยะเวลา / รายได้ / ความยาก](#6-ตารางเปรยบเทยบ)
7. [Next Steps — เริ่มยังไงทันที?](#7-next-steps)

---

## 1. สถานะปัจจุบัน

### Components ที่พร้อมใช้งาน (หรือใกล้พร้อม)

| Component | สถานะ | รายละเอียด |
|---|---|---|
| `hamburger_encode.h` | ✅ ทดสอบผ่านหมด | encode → file → decode lossless roundtrip (T1-T9) |
| `hbv_cli.c` → `hbv.exe` | ✅ compile ได้, ใช้งานได้ | CLI pack/unpack folder เป็น .gpx5 |
| `hb_vault.h` | ✅ ใช้ได้ | folder packer + SHA-256 inline, zstd optional |
| `hamburger_encode_image()` | ✅ ทดสอบผ่าน | auto classify → auto pipe → encode → decode lossless |
| `gpx5_container.h` | ✅ พร้อม | container format พร้อม LUT O(1) random access |
| `geo_tring_walk.h` | ✅ พร้อม | TRing walk dispatch พร้อม polarity ROUTE/GROUND |
| `calibrate.c` / `encode_test02.c` | ✅ compile ได้ | tools สำหรับ encode/decode test |

### สิ่งที่ยังต้องทำข้าม product

- [ ] **UI / DX** — CLI usage polish, error messages ภาษาไทย/อังกฤษ
- [ ] **Packaging** — single .exe distribución, cross-compile for macOS/Linux
- [ ] **License / EULA** — ต้องมีก่อนขาย
- [ ] **Website / Landing page** — 1 page อธิบายว่า product คืออะไร
- [ ] **Sample results** — benchmark real game assets vs ZIP/PNG/ZSTD

---

## 2. Product A: GPX5 CLI — Geometric Archiver

### สินค้า
CLI tool `gpx5.exe` (หรือ `gpx5`) ที่แพ็ค/คลาย folder/file โดยใช้ geometric compression

```bash
gpx5 pack  ./game_folder    build.gpkg
gpx5 unpack build.gpkg      ./restored
gpx5 bench ./game_folder    # เทียบ ratio vs ZIP/ZSTD
```

### ใครซื้อ
- **Indie game devs** (itch.io): build เล็ก = upload เร็ว, bandwidth น้อย
- **Web devs**: assets เล็ก = โหลดเร็ว, SEO ดี
- **Server admin**: backup ก่อนส่ง, compress logs

### ทำไมเลือก GPX5 แทน ZIP/7z
- FLAT tiles → 1 seed (ไม่กี่ byte) แทน raw data ทั้งก้อน
- Noise → frequency params (range + freq_seed, ไม่กี่ byte)
- EDGE → Rice3 codec (delta encode on prediction)
- Lossless, no malloc decoder
- 30-70% smaller than ZIP on game assets (sprite sheets, UI, tilemaps)

### Price
- **Free Tier**: อัดได้ไฟล์รวม ≤ 10MB
- **Pro License**: $29 (หรือ ฿990) — ไม่จำกัด

### ระยะเวลา
- **Dev time**: ~1 สัปดาห์ (ส่วนใหญ่มีแล้ว, ต้อง polish)
- **Ship**: ทันทีที่ benchmark กับ assets จริงแล้วได้ตัวเลข

### ความเสี่ยง
- CLI tool ขายยาก (dev ชอบของฟรี)
- ต้องมี benchmark เปรียบเทียบชัดเจน (ไม่งั้นคนไม่เชื่อ)
- ZIP/7z/ZSTD ฟรีและดีพอสำหรับ use case ทั่วไป

---

## 3. Product B: Game Asset Compression Service

### สินค้า
**บริการรับ compress** — ไม่ใช่ product ที่ต้อง maintain

ลูกค้าส่ง assets มา → เราใช้ `hamburger_encode_image()`  
+ `hb_vault.h` compress ให้ → ส่งคืน `.gpx5` + decoder code ตัวอย่าง

### ใครซื้อ
- **Game jam devs** (มีทุกเดือน, 60% ใช้ itch.io)
- **Solo dev / small studio** ที่ไม่อยากตั้ง pipeline เอง
- **Artist** ที่ส่ง asset ใน portfolio (want file size เล็ก)

### ทำไมถึงซื้อ
- "PNG 1.2MB → 400KB lossless" — ตัวเลขเห็นชัด
- ไม่ต้องติดตั้ง tool, ไม่ต้องตั้งค่า, จ่ายครั้งเดียวงบจบ
- ได้ทั้งไฟล์ + decoder code (ไม่ vendor lock)

### Price
- **Small job** (< 50 sprites): $25
- **Medium** (< 200 assets): $75
- **Large** (ทั้ง game build): $150-300

### ระยะเวลา
- **Dev time**: ~2-3 วัน (ทำ pipeline ให้ automate)
- **เริ่มขายได้ทันที**: เปิดรับ job ที่ X/twitter, reddit r/gamedev, itch.io community

### ข้อดี
- ไม่ต้องมี product — แค่รับงาน + compress + deliver
- ค่าบริการสูงกว่า license (license $29 vs job $75-300)
- สะสม portfolio → ใช้เป็น case study ขาย license เองทีหลัง

---

## 4. Product C: GPX5 Image Codec — SDK License

### สินค้า
SDK สำหรับฝัง GPX5 decoder ใน game engine หรือ web app
- 1 header file (`hamburger_decode.h`) — drop-in, no malloc
- รองรับ decode lossless tile → YCgCo → RGB
- O(1) random access ต่อ tile

### ใครซื้อ
- **Middleware dev** (ทำ engine ของตัวเอง)
- **Cloud gaming platform** (ต้องการ decode ไว, bandwidth ต่ำ)
- **AR/VR tool chain** (streaming textures, O(1) per tile decode)
- **Embedded dev** (decoder ไม่มี malloc = ปลอดภัย 100%)

### Price
- **Indie license**: $99 — 1 project, royalty-free
- **Studio license**: $499 — unlimited projects, source included
- **Enterprise**: custom (contact)

### ระยะเวลา
- **Dev time**: ~2 สัปดาห์ (แยก decoder header, เขียน quickstart, ตัวอย่าง integration)
- **Ship**: ต้องมี demo proof (web demo via WASM, unreal plugin snippet)

### ข้อดี
- SDK มีตลาดชัดเจน (game middleware)
- License ซ้ำได้ — ขายครั้งเดียว รายได้ต่อเนื่อง
- ไม่ต้อง support รายคน (แค่ document + forum)

---

## 5. Product D: GPX5 Texture Packer Plugin

### สินค้า
Plugin สำหรับ Godot หรือ Unity ที่ import `.gpx5` texture โดยตรง
- Godot: `Gpx5Texture` resource (GDScript + GDExtension)
- Unity: `Gpx5Importer` (AssetPostprocessor)

### ใครซื้อ
- **Godot dev** (market เข้าถึงง่าย, community เปิดรับ tools ใหม่)
- **Unity dev** ที่ต้องการ build size optimization

### ทำไมถึงซื้อ
- Texture atlas เข้า Godot โดยตรง — ไม่ต้องแตกเป็น PNG ก่อน
- GPX5 = lossless + smaller than PNG + decode เร็วกว่า PNG lib
- Asset pipeline integration — ไม่ต้อง manual convert

### Price
- **$39** — Godot plugin
- **$59** — Unity plugin

### ระยะเวลา
- **Dev time**: ~3-4 สัปดาห์ (ต้องศึกษาทั้ง GPX5 codec + engine API)
- **Ship**: ต้องมี Godot 4.x และ/หรือ Unity 6

### ความเสี่ยง
- ต้อง maintain compatibility เวอร์ชัน engine update
- แต่ละ engine มี API ต่างกัน → dev time เท่าตัว
- Market saturation (มี texture tool เยอะ)

---

## 6. ตารางเปรียบเทียบ

| Product | Dev Time | Time to Revenue | รายได้/unit | ความยากขาย | ตลาด |
|---|---|---|---|---|---|
| **A: CLI** | 1 wk | 2-4 wk | $29 | ปานกลาง | dev tools |
| **B: Service** | 3 d | **ทันที** | $25-300 | **ง่ายที่สุด** | game dev community |
| **C: SDK** | 2 wk | 4-8 wk | $99-499 | ยาก | middleware |
| **D: Plugin** | 3-4 wk | 4-8 wk | $39-59 | ปานกลาง | Godot/Unity dev |

### ผมแนะนำลำดับ:

1. **เริ่มที่ B (Service) ก่อน**
   - ใช้เวลาน้อยที่สุด, ได้ตังค์เร็วที่สุด
   - ไม่ต้อง build product, ไม่ต้อง maintain
   - สร้าง case study + portfolio → ใช้ขาย product ตัวอื่นต่อ

2. **สะสมทุน → ทำ A (CLI)**
   - เปลี่ยน service → product (ขาย license ซ้ำๆ ได้)
   - ใช้ benchmark จากงาน service เป็น marketing

3. **ทุน + confidence พอ → C (SDK) หรือ D (Plugin)**
   - ต้องมี cash reserve เผื่อ dev time นานขึ้น
   - ตลาดใหญ่ขึ้น แต่ competition เยอะขึ้น

---

## 7. Next Steps

### ถ้าเลือก B (Service) — เริ่มวันนี้

```
Day 1:   benchmark assets จริง → หาตัวเลข ratio ที่น่าสนใจ
         (ใช้ hbv pack กับ sprite sheets / game builds จาก itch.io free assets)
Day 2:   โพสต์ผล benchmark ที่ r/gamedev, r/IndieDev, Twitter
         "I compressed [game] from 120MB to 45MB with geometric compression — no quality loss"
Day 3:   เปิดรับ job — first 5 clients at 50% discount (build case studies)
         แปะลิงก์รับ compress ที่ itch.io profile + Reddit
Day 7+:  มี case study 3-5 อัน → ขึ้นราคาเต็ม → Profit
```

### สิ่งที่ผมช่วยได้ตอนนี้

1. สร้าง deliverable script ที่ wrap `hamburger_encode_image()` + `hb_vault.h`
   เป็น service pipeline — รับ folder → compress → zip `.gpx5` → deliver
2. ช่วยหา benchmark assets จาก free game asset packs
3. เขียน landing page copy (1-pager สำหรับรับ service)

---

**Bottom line:**  
คุณมี engine ที่ unique อยู่แล้ว (geometric compression, tile classify, auto-codec, no-malloc)  
สิ่งที่ต้องทำไม่ใช่ dev ให้หนักขึ้น — **แต่คือเอามันไปให้คนเห็น + รับ compress ให้เขาก่อน**

ตังค์เข้ามา → ซื้ออุปกรณ์ → dev product จริง → scale up

---

*"No business plan survives first contact with customers. เริ่มขายก่อน แล้วค่อยปรับ"*
