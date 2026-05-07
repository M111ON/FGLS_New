Session Handoff — POGLS Wire + Hilbert QR
สิ่งที่เสร็จใน session นี้
tgw_stream_dispatch.h เขียนเสร็จและ deliver แล้ว ปิด SEAM 1+2 พร้อมกัน core คือ tring_route_from_enc(enc) ที่ derive spoke+polarity จาก LUT tring_pos() stateless O(1) ไม่แตะ struct ใดเลย math: pentagon = pos/60, spoke = pentagon%6, polarity = (pos%60) >= 30 มาจาก geometry ล้วนๆ
POGLS SEAM ที่เปิดอยู่
SEAM 3: geo_tring_addr.h include ใน geopixel_v21 แล้ว แต่ decode_full และ encode loop ยังเป็น i%TW, i/TW raster — งานที่เหลือแค่เรียก TRing walk แทน index ตรงนั้น SEAM 4 (GPU batch) และ Layer 1 shell/world awareness ยัง defer test_tgw_stream_dispatch.c ยังไม่ได้เขียน — ต้อง verify spoke coverage uniform 6 spokes, polarity ~50/50, stats consistency

Hilbert QR — New Project (แยกออกมา)
Concept: folder → hb3 encode → base64/zlib stream → Hilbert QR layer → SVG/PNG/TXT ได้ compression 2 ชั้น: zlib (hb3) + Hilbert locality exploit คาด 3-4x บน base64 stream self-repair ฟรีจาก intersection 64 จุดของ Hilbert fixed seed — scan ได้แค่บางส่วนก็ reconstruct ได้ ข้าม platform block ได้ทุก format (.svg/.txt/.md/.png)
งานที่ต้องทำ (เรียงตาม priority)
P1: วัด entropy ของ hb3 output จริงก่อน ถ้า > 6.5 bits/byte compression รอบ 2 ได้แค่ 1.5-2x ไม่ใช่ 4x — ตัวเลขนี้ตัดสินทุกอย่าง P2: Hilbert QR encoder โดยใช้ o23_hilbert() และ o23_build_geo_addrs() จาก geopixel_v21 ที่มีอยู่แล้ว ไม่ต้องเขียนใหม่ P3: fixed seed table — precompute seeds ที่ให้ intersection = 64 เสมอ lock ไว้ใน header 1 byte (seed index) + transformation flags (flip/invert/offset) P4: self-repair decoder — reconstruct เส้นที่ขาดจาก 2 เส้นที่เหลือ O(1) per missing edge P5: hb3 เพิ่ม encode_txt / decode_txt mode สำหรับ Kilo Code workflow
Key insight ที่ต้องจำ
1 เส้นที่ถูกข้ามใน Hilbert curve หนึ่ง ถูก "จำ" โดยอีก 2 เส้นที่ผ่านจุดนั้น และเพราะ Hilbert ต้องครอบทุก cell พอดี 2 เส้นนั้นยืนยัน orientation ของเส้นที่หายได้ทันที ไม่ต้อง search decode O(1) redundancy ไม่มีต้นทุน storage เพราะ geometry สร้างมันเองอยู่แล้ว