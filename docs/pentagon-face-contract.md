# Pentagon Face Contract (PFC)

### A Topology-First Architecture for Goldberg / Geodesic / Polyhedral Meshes

แนวคิดหลักของระบบนี้คือ **แยก Topology ออกจาก Subdivision** โดยถือว่า topology ของทรงกลมมีโครงสร้างถาวร ส่วน mesh ภายในแต่ละหน้าเป็นเพียง implementation ที่เปลี่ยนได้

## Core Invariants (สิ่งที่ห้ามเปลี่ยน)

```text
12 Pentagon Centers
```

จุดศูนย์กลางของ Pentagon ทั้ง 12 คือ Anchor หลักของระบบและไม่เคยเปลี่ยน

```text
12 Faces
5 Neighbor per Face
5 Boundary per Face
```

ความสัมพันธ์ระหว่างหน้า (Adjacency) คงที่ตลอด ไม่ว่าจะใช้ subdivision แบบใด

## Pentagon Face Contract

ทุก Face ต้องมี Interface เดียวกัน

```text
PentagonFace

Center      (fixed)
Corners[5]  (fixed)
Boundary[5] (fixed)
Neighbors[5](fixed)

Internal Mesh = Free
```

กล่าวคือ

- Boundary และ Neighbor คือ Contract
- ส่วน Internal Mesh เป็น Implementation

## Internal Mesh

ภายใน Face สามารถใช้ Generator ได้หลายชนิด

```text
Goldberg Hex
Triangle Fan
Quad Mesh
Mixed Mesh
Adaptive Mesh
AI Generated Mesh
Future Algorithms
```

ทุก Generator สามารถเสียบแทนกันได้ ตราบใดที่ยังรักษา Pentagon Face Contract

## Shared Vertex

ระบบอนุญาตให้เกิด

```text
Triangle ×5
Shared Vertex
```

หรือ topology ภายในรูปแบบอื่นได้

โดยไม่ถือว่าเป็นปัญหา

เพราะสิ่งที่ Runtime สนใจคือ Boundary Contract เท่านั้น

## Runtime View

Runtime ไม่ต้องรู้รายละเอียด subdivision

เพียงทำงานกับ Face Contract

```text
Face
  ↓
Generator
  ↓
Internal Mesh
```

เปลี่ยน Generator ได้โดยไม่ต้องแก้ Runtime

## Global Topology

Topology ของทั้งทรงกลมคงเดิมเสมอ

```text
12 Pentagon Centers
        │
        ▼
Fixed Neighbor Graph
        │
        ▼
Replaceable Internal Mesh
```

ดังนั้น

```text
Topology = Stable
Geometry = Replaceable
Subdivision = Replaceable
Rendering = Replaceable
```

## Design Philosophy

ระบบนี้ไม่ได้ยึดติดกับ Goldberg Polyhedron

แต่ยึดติดกับ

```text
Pentagon Face Contract
```

Goldberg เป็นเพียง Generator ตัวหนึ่ง

เช่นเดียวกับ Triangular, Quad, Adaptive หรือ Generator อื่นในอนาคต

## Architectural Benefits

```text
Stable Topology
Stable Runtime ABI
Modular Face Generator
Independent Subdivision
Future-proof Design
Deterministic Connectivity
```

## สรุป

หัวใจของระบบไม่ใช่การสร้าง Goldberg Polyhedron แต่คือการสร้าง **Pentagon Face Contract (PFC)** ซึ่งกำหนดให้ทรงกลมมีโครงสร้างระดับบน (Topology) ที่คงที่ด้วย Pentagon Center 12 จุด และ Face Interface ที่ตายตัว ขณะที่โครงสร้างภายในแต่ละ Face สามารถเลือกใช้ Subdivision หรือ Mesh Generator แบบใดก็ได้ โดยไม่กระทบต่อ Runtime หรือ Face อื่น ทำให้ระบบสามารถรองรับ Goldberg, Geodesic, Triangle, Quad, Adaptive หรืออัลกอริทึมใหม่ในอนาคตได้ภายใต้สถาปัตยกรรมเดียวกัน
