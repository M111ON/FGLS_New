"""
geometric_concepts.py — Helix, Scaling, Rotation, Internal Tunnel Prototype
═══════════════════════════════════════════════════════════════════════════════

Implements:
1. Helix path on geometry
2. Scaling function for shapes
3. Rotation function
4. Internal tunnel (Decagram) concept
"""

import math
from typing import List, Tuple, Dict, Any
from dataclasses import dataclass


@dataclass
class Point3D:
    """3D point"""
    x: float
    y: float
    z: float


class GeometricConcepts:
    """Geometric concepts: Helix, Scaling, Rotation, Internal Tunnel"""
    
    def __init__(self):
        # Dodecahedron vertices (regular)
        phi = (1 + math.sqrt(5)) / 2  # Golden ratio
        self.dodeca_vertices = [
            Point3D(1, 1, 1), Point3D(1, 1, -1), Point3D(1, -1, 1), Point3D(1, -1, -1),
            Point3D(-1, 1, 1), Point3D(-1, 1, -1), Point3D(-1, -1, 1), Point3D(-1, -1, -1),
            Point3D(0, phi, 1/phi), Point3D(0, phi, -1/phi), Point3D(0, -phi, 1/phi), Point3D(0, -phi, -1/phi),
            Point3D(1/phi, 0, phi), Point3D(1/phi, 0, -phi), Point3D(-1/phi, 0, phi), Point3D(-1/phi, 0, -phi),
            Point3D(phi, 1/phi, 0), Point3D(phi, -1/phi, 0), Point3D(-phi, 1/phi, 0), Point3D(-phi, -1/phi, 0),
        ]
        
        # Decagram vertices (10-pointed star)
        self.decagram_vertices = []
        for i in range(10):
            angle = i * 36 * math.pi / 180
            # Outer points
            self.decagram_vertices.append(Point3D(
                2 * math.cos(angle),
                2 * math.sin(angle),
                0
            ))
            # Inner points
            angle_inner = (i * 36 + 18) * math.pi / 180
            self.decagram_vertices.append(Point3D(
                1 * math.cos(angle_inner),
                1 * math.sin(angle_inner),
                0
            ))
    
    def helix_path(self, n_points: int = 100, radius: float = 1.0, 
                   height: float = 2.0, turns: float = 3.0) -> List[Point3D]:
        """
        Generate helix path on sphere surface.
        
        Parameters:
        - n_points: number of points
        - radius: sphere radius
        - height: total height
        - turns: number of turns
        
        Returns: List of 3D points forming helix
        """
        points = []
        for i in range(n_points):
            t = i / (n_points - 1)
            angle = t * turns * 2 * math.pi
            z = -height/2 + t * height
            r = math.sqrt(radius**2 - z**2) if abs(z) <= radius else 0
            
            x = r * math.cos(angle)
            y = r * math.sin(angle)
            points.append(Point3D(x, y, z))
        
        return points
    
    def scale_shape(self, vertices: List[Point3D], scale: float, 
                    center: Point3D = None) -> List[Point3D]:
        """
        Scale shape by factor.
        
        Parameters:
        - vertices: original vertices
        - scale: scale factor (0.5 = half, 2.0 = double)
        - center: center of scaling (default: origin)
        
        Returns: Scaled vertices
        """
        if center is None:
            center = Point3D(0, 0, 0)
        
        scaled = []
        for v in vertices:
            x = center.x + (v.x - center.x) * scale
            y = center.y + (v.y - center.y) * scale
            z = center.z + (v.z - center.z) * scale
            scaled.append(Point3D(x, y, z))
        
        return scaled
    
    def rotate_shape(self, vertices: List[Point3D], angle_x: float = 0, 
                     angle_y: float = 0, angle_z: float = 0) -> List[Point3D]:
        """
        Rotate shape around axes.
        
        Parameters:
        - vertices: original vertices
        - angle_x: rotation around X axis (radians)
        - angle_y: rotation around Y axis (radians)
        - angle_z: rotation around Z axis (radians)
        
        Returns: Rotated vertices
        """
        rotated = []
        for v in vertices:
            # Rotate around X
            y = v.y * math.cos(angle_x) - v.z * math.sin(angle_x)
            z = v.y * math.sin(angle_x) + v.z * math.cos(angle_x)
            x = v.x
            
            # Rotate around Y
            z_new = z * math.cos(angle_y) - x * math.sin(angle_y)
            x_new = z * math.sin(angle_y) + x * math.cos(angle_y)
            x, z = x_new, z_new
            
            # Rotate around Z
            x_final = x * math.cos(angle_z) - y * math.sin(angle_z)
            y_final = x * math.sin(angle_z) + y * math.cos(angle_z)
            
            rotated.append(Point3D(x_final, y_final, z))
        
        return rotated
    
    def create_internal_tunnel(self, dodeca_vertices: List[Point3D], 
                               decagram_vertices: List[Point3D]) -> Dict[str, Any]:
        """
        Create internal tunnel using Decagram.
        
        This connects external (dodecahedron) with internal (decagram)
        through bi-polar relationship.
        """
        # Find bi-polar points (north/south poles)
        north_pole = Point3D(0, 0, 1)
        south_pole = Point3D(0, 0, -1)
        
        # Create tunnel path from north to south through decagram
        tunnel_path = []
        for i, v in enumerate(decagram_vertices):
            if i % 2 == 0:  # Outer points
                tunnel_path.append(v)
        
        return {
            "north_pole": north_pole,
            "south_pole": south_pole,
            "tunnel_path": tunnel_path,
            "dodeca_vertices": dodeca_vertices,
            "decagram_vertices": decagram_vertices
        }
    
    def demonstrate_helix_with_scaling(self) -> List[Dict[str, Any]]:
        """
        Demonstrate helix formation through scaling + rotation.
        
        Shows how scaling one shape while rotating creates helix pattern.
        """
        results = []
        
        # Start with dodecahedron
        base_shape = self.dodeca_vertices
        
        # Scale and rotate to create helix
        n_steps = 10
        for i in range(n_steps):
            t = i / (n_steps - 1)
            
            # Scale from 1.0 to 0.3
            scale = 1.0 - t * 0.7
            
            # Rotate proportionally
            angle = t * 2 * math.pi
            
            # Apply scaling and rotation
            scaled = self.scale_shape(base_shape, scale)
            rotated = self.rotate_shape(scaled, angle_z=angle)
            
            results.append({
                "step": i,
                "scale": scale,
                "angle": angle,
                "vertices": rotated
            })
        
        return results


def demonstrate_all_concepts():
    """Demonstrate all geometric concepts"""
    
    print("=" * 80)
    print("GEOMETRIC CONCEPTS PROTOTYPE")
    print("=" * 80)
    print()
    
    geo = GeometricConcepts()
    
    # 1. Helix Path
    print("1. HELIX PATH")
    print("-" * 80)
    helix = geo.helix_path(n_points=20, radius=1.0, height=2.0, turns=2.0)
    print(f"   Generated {len(helix)} points")
    print(f"   Start: ({helix[0].x:.2f}, {helix[0].y:.2f}, {helix[0].z:.2f})")
    print(f"   End: ({helix[-1].x:.2f}, {helix[-1].y:.2f}, {helix[-1].z:.2f})")
    print()
    
    # 2. Scaling
    print("2. SCALING")
    print("-" * 80)
    original = geo.dodeca_vertices[:5]  # First 5 vertices
    scaled = geo.scale_shape(original, 0.5)
    print(f"   Original: {len(original)} vertices")
    print(f"   Scaled (0.5x): {len(scaled)} vertices")
    print(f"   Original[0]: ({original[0].x:.2f}, {original[0].y:.2f}, {original[0].z:.2f})")
    print(f"   Scaled[0]: ({scaled[0].x:.2f}, {scaled[0].y:.2f}, {scaled[0].z:.2f})")
    print()
    
    # 3. Rotation
    print("3. ROTATION")
    print("-" * 80)
    rotated = geo.rotate_shape(original, angle_z=math.pi/4)
    print(f"   Rotated 45° around Z")
    print(f"   Rotated[0]: ({rotated[0].x:.2f}, {rotated[0].y:.2f}, {rotated[0].z:.2f})")
    print()
    
    # 4. Internal Tunnel
    print("4. INTERNAL TUNNEL (Decagram)")
    print("-" * 80)
    tunnel = geo.create_internal_tunnel(geo.dodeca_vertices, geo.decagram_vertices)
    print(f"   North pole: ({tunnel['north_pole'].x:.2f}, {tunnel['north_pole'].y:.2f}, {tunnel['north_pole'].z:.2f})")
    print(f"   South pole: ({tunnel['south_pole'].x:.2f}, {tunnel['south_pole'].y:.2f}, {tunnel['south_pole'].z:.2f})")
    print(f"   Tunnel path: {len(tunnel['tunnel_path'])} points")
    print()
    
    # 5. Helix from Scaling + Rotation
    print("5. HELIX FROM SCALING + ROTATION")
    print("-" * 80)
    helix_demo = geo.demonstrate_helix_with_scaling()
    print(f"   Generated {len(helix_demo)} steps")
    print(f"   Scale range: {helix_demo[0]['scale']:.2f} → {helix_demo[-1]['scale']:.2f}")
    print(f"   Angle range: {helix_demo[0]['angle']:.2f} → {helix_demo[-1]['angle']:.2f}")
    print()
    
    # Summary
    print("=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print("""
    ✓ Helix Path — spiral navigation on sphere
    ✓ Scaling — resize shapes (0.5x, 2x, etc.)
    ✓ Rotation — rotate around axes (X, Y, Z)
    ✓ Internal Tunnel — Decagram connecting poles
    ✓ Helix from Scaling + Rotation — automatic helix formation
    
    All concepts are now implemented and ready to use!
    """)
    
    return {
        "helix": helix,
        "scaled": scaled,
        "rotated": rotated,
        "tunnel": tunnel,
        "helix_demo": helix_demo
    }


if __name__ == "__main__":
    results = demonstrate_all_concepts()
