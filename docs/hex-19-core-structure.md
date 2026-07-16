# Hex-19 Core Structure (Metatron-like Cluster)

### A Canonical Hexagonal Core Derived from Pentagon Face Contract

## Overview

โครงสร้างนี้เป็น **Hexagonal Canonical Core** ที่เกิดขึ้นจากการปูกระเบื้องบน Hex Lattice ภายใน Pentagon Face โดยธรรมชาติ ไม่ได้เริ่มจากการสร้าง Metatron Cube แต่เมื่อขยายจากศูนย์กลางด้วยกฎของ Hex Neighbor จะเกิดรูปแบบที่มีสมมาตรใกล้เคียงกับ Metatron Cube

จึงเรียกว่า

```text
Metatron-like Structure
```

ไม่ใช่ Metatron Cube โดยตรง

---

## Topological Construction

เริ่มจากจุดศูนย์กลาง

```text
Radius 0

    ●
```

ขยายเพื่อนบ้านชั้นแรก

```text
Radius 1

      ●
   ●  ●  ●
      ●
   ●     ●
```

ได้

```text
1 + 6 = 7 nodes
```

ขยายอีกหนึ่งชั้น

```text
Radius 2
```

จะได้

```text
1
+6
+12

=19 nodes
```

ซึ่งเป็น Hex Cluster มาตรฐาน

---

## 13-Structure Core

ภายใน Cluster เดียวกัน สามารถเลือกเฉพาะ

```text
Center
+
Inner Ring
+
Six Outer Anchors
```

ได้ทั้งหมด

```text
13 Nodes
```

โครงสร้างนี้มี symmetry เดียวกับ

```text
Metatron Cube
```

ในระดับ Graph Topology

ไม่ใช่ระดับเส้นเรขาคณิต

---

## Canonical Numbers

```text
Center           = 1
Ring 1           = 6
13 Structure     = 13
Hex Cluster      = 19
Orange Modules   = 6
Left Triangles   = 36
```

---

## 19 Cell Cluster

Cluster หลักประกอบด้วย

```text
Radius 0 = 1

Radius 1 = 6

Radius 2 = 12
```

รวม

```text
19 Cells
```

ถือเป็น Canonical Hex Cluster

---

## Six Orange Modules

Orange Cell ทั้งหมด

```text
6 Modules
```

วางสมมาตรรอบแกนกลาง

ทำหน้าที่เป็น

```text
Expansion Interface

หรือ

Neighbor Connectors
```

สำหรับเชื่อมต่อกับ Face รอบข้าง

---

## 36 Residual Triangles

เมื่อสร้าง Hex Cluster แล้ว

พื้นที่ที่เหลือกลายเป็น

```text
36 Triangles
```

จัดเรียงแบบ

```text
6 Sectors
×
6 Triangles
=
36
```

จึงเป็นผลลัพธ์ของ Hex Symmetry

ไม่ใช่ค่าที่กำหนดขึ้นเอง

---

## Structural Interpretation

```text
13 Nodes
↓
Generate
↓
19 Hex Cluster
↓
Leave
↓
36 Triangles
```

โครงสร้างทั้งหมดเกิดจากกฎเดียวกัน

---

## Relationship to Pentagon Face Contract

ภายใน Pentagon Face

```text
Pentagon Boundary
↓
Hex Generator
↓
19 Cluster
↓
13 Core
↓
36 Residual Triangles
```

ดังนั้น

Hex Cluster เป็นเพียง Internal Generator

ไม่ได้เปลี่ยน Pentagon Contract

---

## Design Principles

```text
Topology First
Stable Pentagon Boundary
Stable Center
Stable Neighbor Graph
Replaceable Internal Generator
```

---

## Key Invariants

```text
12 Pentagon Centers
        ↓
13 Core Structure
        ↓
19 Hex Cluster
        ↓
6 Expansion Modules
        ↓
36 Residual Triangles
```

เลขทั้งหมดเป็นผลจากโครงสร้างของ Hex Lattice และการเชื่อมต่อแบบสมมาตร ไม่ใช่ค่าที่กำหนดขึ้นลอย ๆ

---

## Architectural Insight

โครงสร้างนี้ใช้ **13-node Core** เป็น Blueprint ภายในของ Pentagon Face โดยให้ **19-cell Hex Cluster** เป็นโครงสร้างมาตรฐานสำหรับการแบ่งพื้นที่ และมอง **36 Residual Triangles** เป็นพื้นที่สำหรับการเชื่อมต่อหรือการทำ Subdivision เพิ่มเติมในอนาคต ทั้งหมดอยู่ภายใต้ **Pentagon Face Contract (PFC)** เดียวกัน ทำให้สามารถเปลี่ยนวิธีการ Subdivision ได้โดยไม่กระทบ Topology ของทั้งทรงกลม

**สรุปสั้น**

```text
12 Pentagon Centers
        │
        ▼
13 Canonical Core
        │
        ▼
19 Hex Cluster
        │
        ▼
36 Residual Triangles
        │
        ▼
Future Subdivision / Goldberg / Geodesic / Adaptive
```
