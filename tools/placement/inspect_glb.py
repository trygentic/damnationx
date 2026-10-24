#!/usr/bin/env python3
"""Inspect a local GLB, flatten its static scene, and extract original textures.

Requires NumPy and Pillow. Never accesses the network or modifies the input.
The NPZ keeps glTF axes, UV convention, source/world coordinates and materials.
"""

# From Douglas Pan's diablo-3d (https://github.com/douglasopan/diablo-3d, tools/inspect_glb.py, commit e59e05b),
# Sustainable Use License 1.0 (see LICENSE.md). Modified for DamnationX: flatten(allow_skinned=True) inspects skinned
# props in their bind pose; imported as a library by tools/placement/validate_placement.py.


from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import sys
import urllib.parse

import numpy as np
from PIL import Image


COMPONENTS = {
    5120: np.dtype("i1"), 5121: np.dtype("u1"), 5122: np.dtype("<i2"),
    5123: np.dtype("<u2"), 5125: np.dtype("<u4"), 5126: np.dtype("<f4"),
}
ELEMENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


class GlbError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GlbError(message)


def read_glb(path: Path) -> tuple[dict, bytes, list[dict]]:
    data = path.read_bytes()
    require(len(data) >= 20, "GLB header/chunks are missing")
    magic, version, length = struct.unpack_from("<4sII", data)
    require(magic == b"glTF" and version == 2 and length == len(data), "Expected a complete GLB version 2 file")
    document = None
    binary = b""
    chunks = []
    offset = 12
    while offset < len(data):
        require(offset + 8 <= len(data), "Truncated GLB chunk header")
        size, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        require(size % 4 == 0 and offset + size <= len(data), "Invalid GLB chunk size/alignment")
        chunk = data[offset:offset + size]
        chunks.append({"type": kind, "bytes": size})
        if kind == JSON_CHUNK:
            require(document is None and len(chunks) == 1, "JSON must be the first and only GLB JSON chunk")
            document = json.loads(chunk.rstrip(b" \t\r\n\x00").decode("utf-8"))
        elif kind == BIN_CHUNK:
            require(not binary, "Multiple GLB binary chunks are unsupported")
            binary = chunk
        offset += size
    require(isinstance(document, dict) and document.get("asset", {}).get("version") == "2.0", "GLB contains no glTF 2.0 document")
    return document, binary, chunks


class Reader:
    def __init__(self, path: Path, document: dict, binary: bytes):
        self.directory = path.parent.resolve()
        self.document = document
        self.cache = {}
        self.buffers = []
        for index, buffer in enumerate(document.get("buffers", [])):
            if "uri" in buffer:
                data = self.uri_bytes(buffer["uri"])
            else:
                require(index == 0 and binary, "A GLB buffer has neither local URI nor binary data")
                data = binary
            size = int(buffer["byteLength"])
            require(size >= 0 and len(data) >= size, "A glTF buffer is shorter than byteLength")
            self.buffers.append(data[:size])

    def uri_bytes(self, uri: str) -> bytes:
        if uri.startswith("data:"):
            header, separator, value = uri.partition(",")
            require(separator and ";base64" in header, "Only base64 data URIs are supported")
            return base64.b64decode(value, validate=True)
        parsed = urllib.parse.urlsplit(uri)
        require(not parsed.scheme and not parsed.netloc, "Offline inspector does not fetch external URLs")
        path = (self.directory / urllib.parse.unquote(parsed.path)).resolve()
        require(path.is_relative_to(self.directory), "A glTF dependency is outside its model directory")
        return path.read_bytes()

    def view(self, index: int) -> tuple[bytes, dict]:
        views = self.document.get("bufferViews", [])
        require(0 <= index < len(views), "Invalid bufferView index")
        view = views[index]
        require("EXT_meshopt_compression" not in view.get("extensions", {}), "meshopt compression requires a dedicated decoder")
        buffer = self.buffers[int(view["buffer"])]
        offset = int(view.get("byteOffset", 0))
        length = int(view["byteLength"])
        require(offset >= 0 and length >= 0 and offset + length <= len(buffer), "bufferView extends beyond its buffer")
        return buffer[offset:offset + length], view

    @staticmethod
    def layout(element: str, dtype: np.dtype) -> tuple[list[int], int]:
        require(element in ELEMENTS, "Unknown accessor element type")
        if element.startswith("MAT"):
            dimension = int(element[-1])
            column_stride = (dimension * dtype.itemsize + 3) // 4 * 4
            offsets = [column * column_stride + row * dtype.itemsize
                       for column in range(dimension) for row in range(dimension)]
            return offsets, dimension * column_stride
        return list(range(0, ELEMENTS[element] * dtype.itemsize, dtype.itemsize)), ELEMENTS[element] * dtype.itemsize

    def array(self, view_index: int, offset: int, count: int, element: str, component: int) -> np.ndarray:
        require(component in COMPONENTS and count >= 0, "Invalid accessor count/component type")
        dtype = COMPONENTS[component]
        offsets, element_size = self.layout(element, dtype)
        data, view = self.view(view_index)
        stride = int(view.get("byteStride", element_size))
        require(stride >= element_size and offset >= 0, "Invalid accessor byteOffset/byteStride")
        end = offset if count == 0 else offset + (count - 1) * stride + offsets[-1] + dtype.itemsize
        require(end <= len(data), "Accessor extends beyond its bufferView")
        values = np.empty((count, len(offsets)), dtype=dtype)
        for column, component_offset in enumerate(offsets):
            if count:
                values[:, column] = np.ndarray((count,), dtype=dtype, buffer=data,
                                              offset=offset + component_offset, strides=(stride,))
        return values

    def accessor(self, index: int) -> np.ndarray:
        if index in self.cache:
            return self.cache[index]
        accessors = self.document.get("accessors", [])
        require(0 <= index < len(accessors), "Invalid accessor index")
        accessor = accessors[index]
        count, element, component = int(accessor["count"]), accessor["type"], int(accessor["componentType"])
        require(component in COMPONENTS and element in ELEMENTS, "Unknown accessor type")
        if "bufferView" in accessor:
            values = self.array(int(accessor["bufferView"]), int(accessor.get("byteOffset", 0)), count, element, component)
        else:
            values = np.zeros((count, ELEMENTS[element]), dtype=COMPONENTS[component])
        sparse = accessor.get("sparse")
        if sparse:
            sparse_count = int(sparse["count"])
            indices = sparse["indices"]
            require(int(indices["componentType"]) in (5121, 5123, 5125), "Sparse indices must be unsigned integers")
            positions = self.array(int(indices["bufferView"]), int(indices.get("byteOffset", 0)), sparse_count,
                                   "SCALAR", int(indices["componentType"])).reshape(-1).astype(np.int64)
            require(np.all(positions >= 0) and np.all(positions < count)
                    and np.all(np.diff(positions) > 0), "Sparse accessor indices are out of range or unordered")
            replacement = sparse["values"]
            values[positions] = self.array(int(replacement["bufferView"]), int(replacement.get("byteOffset", 0)),
                                           sparse_count, element, component)
        if accessor.get("normalized", False) and component != 5126:
            info = np.iinfo(COMPONENTS[component])
            values = values.astype(np.float64) / info.max
            if info.min < 0:
                values = np.maximum(values, -1.0)
        self.cache[index] = values
        return values


def node_matrix(node: dict) -> np.ndarray:
    if "matrix" in node:
        require(len(node["matrix"]) == 16, "Node matrix must contain 16 numbers")
        matrix = np.array(node["matrix"], dtype=np.float64).reshape(4, 4, order="F")
    else:
        translation = np.asarray(node.get("translation", [0, 0, 0]), dtype=np.float64)
        scale = np.asarray(node.get("scale", [1, 1, 1]), dtype=np.float64)
        quaternion = np.asarray(node.get("rotation", [0, 0, 0, 1]), dtype=np.float64)
        require(translation.shape == (3,) and scale.shape == (3,) and quaternion.shape == (4,), "Invalid node TRS dimensions")
        norm = np.linalg.norm(quaternion)
        require(norm > 0, "Node quaternion has zero length")
        x, y, z, w = quaternion / norm
        rotation = np.array([
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
        ], dtype=np.float64)
        matrix = np.eye(4, dtype=np.float64)
        matrix[:3, :3] = rotation @ np.diag(scale)
        matrix[:3, 3] = translation
    require(np.all(np.isfinite(matrix)), "Node transform contains nonfinite numbers")
    require(np.allclose(matrix[3], [0, 0, 0, 1]), "Node transform is not affine")
    return matrix


def triangles(indices: np.ndarray, mode: int) -> np.ndarray:
    indices = indices.reshape(-1).astype(np.int64)
    if mode == 4:
        require(len(indices) % 3 == 0, "Triangle index count is not divisible by three")
        result = indices.reshape(-1, 3)
    elif mode == 5:
        result = np.array([[indices[i + (i % 2)], indices[i + (1 - i % 2)], indices[i + 2]]
                           for i in range(max(0, len(indices) - 2))], dtype=np.int64).reshape(-1, 3)
    elif mode == 6:
        result = np.array([[indices[0], indices[i], indices[i + 1]]
                           for i in range(1, len(indices) - 1)], dtype=np.int64).reshape(-1, 3)
    else:
        return np.empty((0, 3), dtype=np.int64)
    return result[(result[:, 0] != result[:, 1]) & (result[:, 1] != result[:, 2]) & (result[:, 2] != result[:, 0])]


def base_color_info(document: dict, material_id: int) -> dict:
    materials = document.get("materials", [])
    if material_id < 0:
        return {}
    require(material_id < len(materials), "Primitive material index is invalid")
    return materials[material_id].get("pbrMetallicRoughness", {}).get("baseColorTexture", {})


def flatten(reader: Reader, allow_skinned: bool = False) -> tuple[dict[str, np.ndarray], list[dict], list[str]]:
    document = reader.document
    nodes = document.get("nodes", [])
    meshes = document.get("meshes", [])
    scenes = document.get("scenes", [])
    if scenes:
        scene_index = int(document.get("scene", 0))
        require(0 <= scene_index < len(scenes), "Default scene index is invalid")
        roots = scenes[scene_index].get("nodes", [])
    elif nodes:
        children = {child for node in nodes for child in node.get("children", [])}
        roots = [index for index in range(len(nodes)) if index not in children]
    else:
        nodes = [{"mesh": index} for index in range(len(meshes))]
        roots = list(range(len(nodes)))
    arrays = {key: [] for key in ("world_vertices", "triangles", "uv", "source_uv", "normals", "colors", "uv_present", "normals_present", "material_ids", "primitive_ids")}
    primitives = []
    warnings = []
    vertex_offset = 0
    triangle_offset = 0

    def visit(index: int, parent: np.ndarray, ancestors: set[int]) -> None:
        nonlocal vertex_offset, triangle_offset
        require(0 <= index < len(nodes) and index not in ancestors, "Node hierarchy has an invalid index or cycle")
        node = nodes[index]
        world = parent @ node_matrix(node)
        if "skin" in node:
            if not allow_skinned:
                raise GlbError("Skinned models need pose evaluation; this inspector accepts static scenery")
            # DamnationX: bind pose, as the game's static mesh loader draws a skinned prop. glTF ignores the
            # transform of a skinned mesh's node; its vertices are already in the bind pose.
            world = np.eye(4)
            warnings.append(f"Node {index} is skinned: inspected in its bind pose")
        if "mesh" in node:
            mesh_index = int(node["mesh"])
            require(0 <= mesh_index < len(meshes), "Node mesh index is invalid")
            mesh = meshes[mesh_index]
            for local_index, primitive in enumerate(mesh.get("primitives", [])):
                require("KHR_draco_mesh_compression" not in primitive.get("extensions", {}), "Draco compression requires a dedicated decoder")
                mode = int(primitive.get("mode", 4))
                if mode not in (4, 5, 6):
                    warnings.append(f"Skipped nontriangle primitive node {index}, primitive {local_index}")
                    continue
                attributes = primitive.get("attributes", {})
                require("POSITION" in attributes, "Triangle primitive has no positions")
                positions = reader.accessor(int(attributes["POSITION"])).astype(np.float64)
                require(positions.ndim == 2 and positions.shape[1] == 3 and len(positions) > 0, "Positions must be nonempty VEC3")
                count = len(positions)
                homogeneous = np.column_stack([positions, np.ones(count)])
                transformed = (world @ homogeneous.T).T[:, :3]
                require(np.all(np.isfinite(transformed)), "Positions contain nonfinite numbers")
                source_indices = reader.accessor(int(primitive["indices"])) if "indices" in primitive else np.arange(count)
                faces = triangles(source_indices, mode)
                require(len(faces) > 0 and np.all(faces >= 0) and np.all(faces < count), "Primitive triangle indices are empty or out of range")
                if np.linalg.det(world[:3, :3]) < 0:
                    faces = faces[:, [0, 2, 1]]
                material_id = int(primitive.get("material", -1))
                texture = base_color_info(document, material_id)
                transform = texture.get("extensions", {}).get("KHR_texture_transform", {})
                uv_set = int(transform.get("texCoord", texture.get("texCoord", 0)))
                uv_attribute = f"TEXCOORD_{uv_set}"
                has_uv = uv_attribute in attributes
                raw_uv = reader.accessor(int(attributes[uv_attribute])).astype(np.float64) if has_uv else np.zeros((count, 2))
                require(raw_uv.shape == (count, 2), "Texture coordinates do not match the primitive vertices")
                uv = raw_uv.copy()
                if transform:
                    scale = np.asarray(transform.get("scale", [1, 1]), dtype=np.float64)
                    offset = np.asarray(transform.get("offset", [0, 0]), dtype=np.float64)
                    angle = float(transform.get("rotation", 0))
                    rotation = np.array([[math.cos(angle), -math.sin(angle)], [math.sin(angle), math.cos(angle)]])
                    uv = (uv * scale) @ rotation.T + offset
                if texture and not has_uv:
                    warnings.append(f"Material {material_id} has a base-color texture but primitive {len(primitives)} lacks {uv_attribute}")
                has_normals = "NORMAL" in attributes
                normals = reader.accessor(int(attributes["NORMAL"])).astype(np.float64) if has_normals else np.zeros((count, 3))
                require(normals.shape == (count, 3), "Normals do not match the primitive vertices")
                if has_normals:
                    require(abs(np.linalg.det(world[:3, :3])) > 1e-20, "Normal transform is singular")
                    normals = normals @ np.linalg.inv(world[:3, :3])
                    lengths = np.linalg.norm(normals, axis=1, keepdims=True)
                    normals = np.divide(normals, lengths, out=np.zeros_like(normals), where=lengths > 1e-20)
                colors = np.ones((count, 4), dtype=np.float64)
                if "COLOR_0" in attributes:
                    source_colors = reader.accessor(int(attributes["COLOR_0"]))
                    require(source_colors.shape in ((count, 3), (count, 4)), "Vertex color shape is invalid")
                    colors[:, :source_colors.shape[1]] = source_colors
                if primitive.get("targets"):
                    warnings.append(f"Primitive {len(primitives)} has morph targets; exported geometry uses its base shape")
                primitive_id = len(primitives)
                arrays["world_vertices"].append(transformed.astype(np.float32))
                arrays["triangles"].append((faces + vertex_offset).astype(np.uint32))
                arrays["source_uv"].append(raw_uv.astype(np.float32))
                arrays["uv"].append(uv.astype(np.float32))
                arrays["normals"].append(normals.astype(np.float32))
                arrays["colors"].append(colors.astype(np.float32))
                arrays["uv_present"].append(np.full(count, has_uv, dtype=np.bool_))
                arrays["normals_present"].append(np.full(count, has_normals, dtype=np.bool_))
                arrays["material_ids"].append(np.full(len(faces), material_id, dtype=np.int32))
                arrays["primitive_ids"].append(np.full(len(faces), primitive_id, dtype=np.int32))
                primitives.append({"node": index, "nodeName": node.get("name"), "mesh": mesh_index,
                                   "meshName": mesh.get("name"), "primitive": local_index, "material": material_id,
                                   "vertexRange": [vertex_offset, vertex_offset + count],
                                   "triangleRange": [triangle_offset, triangle_offset + len(faces)],
                                   "texCoordSet": uv_set, "uvPresent": has_uv, "normalsPresent": has_normals,
                                   "worldMatrix": world.tolist()})
                vertex_offset += count
                triangle_offset += len(faces)
        for child in node.get("children", []):
            visit(int(child), world, ancestors | {index})

    for root in roots:
        visit(int(root), np.eye(4), set())
    require(arrays["world_vertices"], "The selected static scene contains no triangle mesh")
    output = {key: np.concatenate(value, axis=0) for key, value in arrays.items()}
    positions = output["world_vertices"].astype(np.float64)
    minimum, maximum = positions.min(axis=0), positions.max(axis=0)
    extent = maximum - minimum
    largest = float(extent.max())
    require(largest > 1e-12, "The model has zero spatial extent")
    origin = np.array([(minimum[0] + maximum[0]) / 2, minimum[1], (minimum[2] + maximum[2]) / 2])
    normalize = np.eye(4)
    normalize[:3, :3] *= 1 / largest
    normalize[:3, 3] = -origin / largest
    output["vertices"] = ((positions - origin) / largest).astype(np.float32)
    output["world_to_normalized"] = normalize
    output["normalized_to_world"] = np.linalg.inv(normalize)
    output["primitive_vertex_ranges"] = np.asarray([item["vertexRange"] for item in primitives], dtype=np.uint32)
    output["primitive_triangle_ranges"] = np.asarray([item["triangleRange"] for item in primitives], dtype=np.uint32)
    if document.get("animations"):
        warnings.append("Animations are not evaluated; exported geometry uses static node transforms")
    return output, primitives, warnings


def extract_materials(reader: Reader, out_dir: Path) -> tuple[list[dict], list[dict]]:
    document = reader.document
    image_metadata = {}
    materials = []
    for index, material in enumerate(document.get("materials", [])):
        pbr = material.get("pbrMetallicRoughness", {})
        info = pbr.get("baseColorTexture")
        record = {"index": index, "name": material.get("name"),
                  "baseColorFactor": pbr.get("baseColorFactor", [1, 1, 1, 1]),
                  "metallicFactor": pbr.get("metallicFactor", 1), "roughnessFactor": pbr.get("roughnessFactor", 1),
                  "doubleSided": material.get("doubleSided", False), "alphaMode": material.get("alphaMode", "OPAQUE"),
                  "alphaCutoff": material.get("alphaCutoff", 0.5), "extensions": material.get("extensions", {})}
        if info:
            texture_index = int(info["index"])
            texture = document["textures"][texture_index]
            source = texture.get("source")
            for extension in ("KHR_texture_basisu", "EXT_texture_webp"):
                source = texture.get("extensions", {}).get(extension, {}).get("source", source)
            require(isinstance(source, int), "Base-color texture has no image source")
            if source not in image_metadata:
                image = document["images"][source]
                if "bufferView" in image:
                    data, _ = reader.view(int(image["bufferView"]))
                    location = "embedded-bufferView"
                else:
                    require("uri" in image, "Image has no bufferView or URI")
                    data = reader.uri_bytes(image["uri"])
                    location = "data-uri" if image["uri"].startswith("data:") else "local-uri"
                mime = image.get("mimeType")
                if data.startswith(b"\x89PNG\r\n\x1a\n"):
                    suffix = ".png"
                elif data.startswith(b"\xff\xd8\xff"):
                    suffix = ".jpg"
                elif data.startswith(b"RIFF") and data[8:12] == b"WEBP":
                    suffix = ".webp"
                elif data.startswith(b"\xabKTX 20\xbb\r\n\x1a\n"):
                    suffix = ".ktx2"
                else:
                    suffix = ".bin"
                texture_dir = out_dir / "textures"
                texture_dir.mkdir(parents=True, exist_ok=True)
                target = texture_dir / (f"image-{source}" + suffix)
                target.write_bytes(data)  # Exact bytes, with no decode/re-encode or painting.
                image_record = {"index": source, "name": image.get("name"), "source": location,
                                "mimeType": mime, "path": str(target), "bytes": len(data),
                                "sha256": hashlib.sha256(data).hexdigest()}
                try:
                    with Image.open(io.BytesIO(data)) as decoded:
                        image_record.update({"width": decoded.width, "height": decoded.height, "mode": decoded.mode})
                except (OSError, ValueError):
                    image_record["imageDecoderUnavailable"] = True
                image_metadata[source] = image_record
            record["baseColorTexture"] = {"texture": texture_index, "image": source,
                                          "texCoord": info.get("texCoord", 0),
                                          "transform": info.get("extensions", {}).get("KHR_texture_transform"),
                                          "sampler": document.get("samplers", [])[texture["sampler"]] if "sampler" in texture else {},
                                          "path": image_metadata[source]["path"]}
        materials.append(record)
    return materials, list(image_metadata.values())


def bounds(vertices: np.ndarray) -> dict:
    minimum, maximum = vertices.min(axis=0), vertices.max(axis=0)
    return {"min": minimum.tolist(), "max": maximum.tolist(), "extent": (maximum - minimum).tolist()}


def geometric_components(vertices: np.ndarray, faces: np.ndarray) -> tuple[np.ndarray, dict]:
    # UV/material seams duplicate vertices. Weld only coincident positions for
    # this connectivity measurement, without changing the exported mesh/UVs.
    tolerance = max(float(np.ptp(vertices, axis=0).max()) * 1e-6, 1e-9)
    _, welded = np.unique(np.rint(vertices / tolerance).astype(np.int64), axis=0, return_inverse=True)
    welded_faces = welded[faces]
    parent = np.arange(int(welded.max()) + 1, dtype=np.int64)
    rank = np.zeros(len(parent), dtype=np.uint8)

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = int(parent[index])
        return index

    def union(left: int, right: int) -> None:
        left, right = find(left), find(right)
        if left == right:
            return
        if rank[left] < rank[right]:
            left, right = right, left
        parent[right] = left
        if rank[left] == rank[right]:
            rank[left] += 1

    for a, b, c in welded_faces:
        union(int(a), int(b))
        union(int(a), int(c))
    roots = np.fromiter((find(int(index)) for index in welded_faces[:, 0]), dtype=np.int64, count=len(faces))
    unique, inverse, counts = np.unique(roots, return_inverse=True, return_counts=True)
    order = np.argsort(-counts)
    remap = np.empty(len(order), dtype=np.int32)
    remap[order] = np.arange(len(order), dtype=np.int32)
    identifiers = remap[inverse]
    components = []
    # Bounds for the largest parts suffice for inspection without producing a
    # huge report for models containing thousands of disconnected tiny details.
    for component_id, old_id in enumerate(order[:100]):
        selected = faces[inverse == old_id]
        points = np.unique(selected.reshape(-1))
        components.append({"id": component_id, "triangles": int(counts[old_id]),
                           "sourceVertices": len(points), "worldBounds": bounds(vertices[points])})
    return identifiers, {"count": len(unique), "positionWeldTolerance": tolerance,
                         "meaning": "Geometric connectivity after coincident-position weld; components are not semantic object labels",
                         "largestComponents": components}


def inspect(path: Path, out_dir: Path) -> dict:
    require(path.is_file(), "The GLB input file does not exist")
    document, binary, chunks = read_glb(path)
    reader = Reader(path, document, binary)
    arrays, primitives, warnings = flatten(reader)
    arrays["component_ids"], components = geometric_components(arrays["world_vertices"], arrays["triangles"])
    out_dir.mkdir(parents=True, exist_ok=True)
    materials, images = extract_materials(reader, out_dir)
    npz_path = out_dir / "mesh-normalized.npz"
    np.savez_compressed(npz_path, **arrays)
    report = {
        "schemaVersion": 1, "input": str(path), "inputBytes": path.stat().st_size,
        "inputSha256": hashlib.sha256(path.read_bytes()).hexdigest(), "asset": document.get("asset"),
        "chunks": chunks, "extensionsUsed": document.get("extensionsUsed", []),
        "extensionsRequired": document.get("extensionsRequired", []),
        "coordinateSystem": {"up": "+Y", "front": "+Z", "handedness": "right",
                             "uvOrigin": "top-left; V is preserved without flipping"},
        "vertexCount": len(arrays["vertices"]), "triangleCount": len(arrays["triangles"]),
        "worldBounds": bounds(arrays["world_vertices"]), "normalizedBounds": bounds(arrays["vertices"]),
        "normalization": {"kind": "uniform; longest dimension 1; XZ centered; minimum Y 0; yaw preserved",
                          "worldToNormalized": arrays["world_to_normalized"].tolist(),
                          "normalizedToWorld": arrays["normalized_to_world"].tolist()},
        "primitives": primitives, "components": components, "materials": materials, "images": images, "warnings": warnings,
        "intermediate": {"path": str(npz_path), "arrays": {key: {"shape": list(value.shape), "dtype": str(value.dtype)}
                                                                       for key, value in arrays.items()},
                         "uv": "Effective base-color UV including KHR_texture_transform; source_uv keeps the original accessor",
                         "normals": "Original normals transformed to world space; missing normals are zero with normals_present=false"},
    }
    (out_dir / "inspection.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {"input": str(path), "vertices": report["vertexCount"], "triangles": report["triangleCount"],
            "materials": len(materials), "components": components["count"], "extractedImages": len(images), "bounds": report["worldBounds"],
            "report": str(out_dir / "inspection.json"), "intermediate": str(npz_path), "warnings": warnings}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("filepath", type=Path)
    parser.add_argument("--outDir", "--out-dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = inspect(args.filepath.resolve(), args.outDir.resolve())
        print(json.dumps(report, ensure_ascii=False))
        return 0
    except (GlbError, OSError, ValueError, KeyError, IndexError, TypeError, struct.error) as error:
        print(json.dumps({"error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
