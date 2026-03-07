"""
Blender 5.0 Python Script: Create 3D Logo from PNG
Creates a textured box with depth and alpha transparency.
"""

import bpy
import bmesh
import os

# Configuration
INPUT_IMAGE = r"C:\Users\julia\Dropbox\AirPixel-White-01.png"
OUTPUT_FILE = r"C:\Users\julia\Dropbox\Claude Code\Graphics test\assets\airpixel_logo.glb"
LOGO_WIDTH = 2.0
LOGO_DEPTH = 0.15  # Depth/thickness of the logo

def clean_scene():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)

    for block in bpy.data.meshes:
        if block.users == 0:
            bpy.data.meshes.remove(block)
    for block in bpy.data.materials:
        if block.users == 0:
            bpy.data.materials.remove(block)
    for block in bpy.data.images:
        if block.users == 0:
            bpy.data.images.remove(block)

def create_logo():
    print("=" * 50)
    print("3D Logo Creator")
    print("=" * 50)

    clean_scene()

    if not os.path.exists(INPUT_IMAGE):
        print(f"ERROR: Image not found: {INPUT_IMAGE}")
        return False

    output_dir = os.path.dirname(OUTPUT_FILE)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir)

    # Load image
    print(f"Loading image: {INPUT_IMAGE}")
    img = bpy.data.images.load(INPUT_IMAGE)
    width, height = img.size
    aspect = width / height
    print(f"Image size: {width}x{height}, aspect: {aspect:.2f}")

    # Calculate dimensions
    plane_width = LOGO_WIDTH
    plane_height = LOGO_WIDTH / aspect
    depth = LOGO_DEPTH

    hw = plane_width / 2
    hh = plane_height / 2
    hd = depth / 2

    # Create a box with depth
    bm = bmesh.new()

    # Front face vertices (z = +hd)
    v0 = bm.verts.new((-hw, -hh, hd))
    v1 = bm.verts.new((hw, -hh, hd))
    v2 = bm.verts.new((hw, hh, hd))
    v3 = bm.verts.new((-hw, hh, hd))

    # Back face vertices (z = -hd)
    v4 = bm.verts.new((-hw, -hh, -hd))
    v5 = bm.verts.new((hw, -hh, -hd))
    v6 = bm.verts.new((hw, hh, -hd))
    v7 = bm.verts.new((-hw, hh, -hd))

    bm.verts.ensure_lookup_table()

    # Create faces
    front_face = bm.faces.new([v0, v1, v2, v3])  # Front (textured)
    back_face = bm.faces.new([v7, v6, v5, v4])   # Back (textured, reversed winding)

    # Side faces (will use solid color)
    bottom_face = bm.faces.new([v4, v5, v1, v0])  # Bottom
    top_face = bm.faces.new([v3, v2, v6, v7])     # Top
    left_face = bm.faces.new([v4, v0, v3, v7])    # Left
    right_face = bm.faces.new([v1, v5, v6, v2])   # Right

    # UV mapping for front and back faces (textured)
    uv_layer = bm.loops.layers.uv.new("UVMap")

    # Front face UVs
    for loop in front_face.loops:
        vert = loop.vert
        u = (vert.co.x + hw) / plane_width
        v = (vert.co.y + hh) / plane_height
        loop[uv_layer].uv = (u, v)

    # Back face UVs (mirrored)
    for loop in back_face.loops:
        vert = loop.vert
        u = 1.0 - (vert.co.x + hw) / plane_width  # Mirror horizontally
        v = (vert.co.y + hh) / plane_height
        loop[uv_layer].uv = (u, v)

    # Side faces get simple UVs (will appear as solid color from edge of texture)
    for face in [bottom_face, top_face, left_face, right_face]:
        for loop in face.loops:
            loop[uv_layer].uv = (0.5, 0.5)  # Center of texture (white area)

    mesh = bpy.data.meshes.new("LogoMesh")
    bm.to_mesh(mesh)
    bm.free()

    obj = bpy.data.objects.new("AirPixelLogo", mesh)
    bpy.context.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)

    # Create material with alpha masking
    print("Creating material...")
    mat = bpy.data.materials.new(name="LogoMaterial")
    mat.use_nodes = True
    mat.blend_method = 'CLIP'  # Alpha clip/mask
    mat.alpha_threshold = 0.1  # Low threshold to catch semi-transparent edges

    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()

    # Output node
    output = nodes.new('ShaderNodeOutputMaterial')
    output.location = (400, 0)

    # Principled BSDF
    bsdf = nodes.new('ShaderNodeBsdfPrincipled')
    bsdf.location = (100, 0)
    bsdf.inputs['Roughness'].default_value = 0.3
    bsdf.inputs['Metallic'].default_value = 0.0
    # Add some emission so it doesn't go dark
    bsdf.inputs['Emission Strength'].default_value = 0.2

    # Texture
    tex_image = nodes.new('ShaderNodeTexImage')
    tex_image.location = (-300, 0)
    tex_image.image = img
    tex_image.interpolation = 'Linear'  # Smoother than Closest

    # Emission color from texture
    emission = nodes.new('ShaderNodeEmission')
    emission.location = (-100, -200)

    links.new(tex_image.outputs['Color'], bsdf.inputs['Base Color'])
    links.new(tex_image.outputs['Color'], bsdf.inputs['Emission Color'])
    links.new(tex_image.outputs['Alpha'], bsdf.inputs['Alpha'])
    links.new(bsdf.outputs['BSDF'], output.inputs['Surface'])

    obj.data.materials.append(mat)

    # Export
    print(f"Exporting to: {OUTPUT_FILE}")
    bpy.ops.export_scene.gltf(
        filepath=OUTPUT_FILE,
        export_format='GLB',
        use_selection=True,
        export_apply=True,
        export_materials='EXPORT'
    )

    print("=" * 50)
    print("SUCCESS!")
    print(f"Output: {OUTPUT_FILE}")
    print("=" * 50)

    return True

if __name__ == "__main__":
    create_logo()
