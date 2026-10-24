#!/usr/bin/env python3
"""Builds the DamnationX 3D pack zips for Discord distribution (docs/3d-asset-packs.md).

Two separate packs, each from a source definition in tools/packs/3d/:

  free  Douglas Opan's diablo-3d models (Sustainable Use License, NOTICE.md)  -> hd/3d/free/
  hd    the HD pack's 3D content (Patreon): props and the 3D warrior        -> hd/3d/hd/, hd/3d/warrior/

The models come from assets/downloaded/ (never committed). Each embedded texture is re-encoded as PNG, downscaled to
--max-texture if larger, because every game build decodes PNG but not all decode JPEG. Each pack gets a manifest.json
(ids, source file, scale, pivot, collision footprint, tier, bounds) and the placement files the game reads
(hd/3d/<tier>/placements/<level type>.ini, docs/design/static-mesh-placement.md), generated from tools/packs/3d/placement.json.
Output goes to assets/packs/ (gitignored):

  assets/packs/damnationx-3d-free-<version>.zip
  assets/packs/damnationx-3d-hd-<version>.zip
  assets/packs/SHA256SUMS

Players unzip a pack next to DIABDAT.MPQ (or wherever their hd folder is); the zips hold the hd/... paths.

Usage:
  tools/build_3d_packs.py                      build both packs
  tools/build_3d_packs.py --pack free          only the FREE pack
  tools/build_3d_packs.py --install DIR        also copy the packs into DIR (a folder holding hd/, or a browser build)
  tools/build_3d_packs.py --hd-art DIR         merge an HD art folder (sprites.txt, ui.txt, ...) into the HD zip

Needs Pillow (pip install pillow).
"""

import argparse
import datetime
import hashlib
import io
import json
import math
import pathlib
import struct
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PACK_SOURCES = ROOT / 'tools' / 'packs' / '3d'
ASSETS = ROOT / 'assets'
DEFAULT_SOURCE = ASSETS / 'downloaded'
DEFAULT_OUT = ASSETS / 'packs'

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942


def read_glb(data: bytes):
    magic, version, length = struct.unpack_from('<III', data, 0)
    if magic != GLB_MAGIC or version != 2:
        raise ValueError('not a glTF 2.0 binary')
    offset = 12
    document = None
    binary = b''
    while offset < length:
        chunk_length, chunk_type = struct.unpack_from('<II', data, offset)
        chunk = data[offset + 8:offset + 8 + chunk_length]
        if chunk_type == CHUNK_JSON:
            document = json.loads(chunk.decode('utf-8'))
        elif chunk_type == CHUNK_BIN:
            binary = bytes(chunk)
        offset += 8 + chunk_length
    if document is None:
        raise ValueError('no JSON chunk')
    return document, binary


def write_glb(document, binary: bytes) -> bytes:
    text = json.dumps(document, separators=(',', ':')).encode('utf-8')
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    length = 12 + 8 + len(text) + (8 + len(binary) if binary else 0)
    out = struct.pack('<III', GLB_MAGIC, 2, length) + struct.pack('<II', len(text), CHUNK_JSON) + text
    if binary:
        out += struct.pack('<II', len(binary), CHUNK_BIN) + binary
    return out


def reencode_textures(document, binary: bytes, max_texture: int):
    """Re-encodes every embedded image as PNG (downscaled to max_texture) and repacks the binary chunk."""
    from PIL import Image

    views = document.get('bufferViews', [])
    replaced = {}
    for image in document.get('images', []):
        if 'bufferView' not in image:
            continue
        view = views[image['bufferView']]
        start = view.get('byteOffset', 0)
        source = binary[start:start + view['byteLength']]
        with Image.open(io.BytesIO(source)) as decoded:
            decoded.load()
            picture = decoded.convert('RGBA' if decoded.mode in ('RGBA', 'LA', 'P') else 'RGB')
        if max(picture.size) > max_texture:
            ratio = max_texture / max(picture.size)
            picture = picture.resize((max(1, round(picture.width * ratio)), max(1, round(picture.height * ratio))), Image.LANCZOS)
        encoded = io.BytesIO()
        picture.save(encoded, format='PNG', optimize=True)
        replaced[image['bufferView']] = encoded.getvalue()
        image['mimeType'] = 'image/png'

    # Rebuild the binary chunk with every buffer view in order, 4-byte aligned (8 for safety with doubles).
    out = bytearray()
    for index, view in enumerate(views):
        if view.get('buffer', 0) != 0:
            raise ValueError('only single-buffer GLBs are supported')
        payload = replaced.get(index)
        if payload is None:
            start = view.get('byteOffset', 0)
            payload = binary[start:start + view['byteLength']]
        out += b'\0' * (-len(out) % 8)
        view['byteOffset'] = len(out)
        view['byteLength'] = len(payload)
        out += payload
    if document.get('buffers'):
        document['buffers'][0]['byteLength'] = len(out)
        document['buffers'][0].pop('uri', None)
    return bytes(out)


def node_matrix(node):
    if 'matrix' in node:
        m = node['matrix']
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    t = node.get('translation', [0, 0, 0])
    x, y, z, w = node.get('rotation', [0, 0, 0, 1])
    s = node.get('scale', [1, 1, 1])
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[r[i][j] * s[j] for j in range(3)] + [t[i]] for i in range(3)] + [[0, 0, 0, 1]]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inspect(document):
    """Bounds (with node transforms; skinned meshes in bind space, as the game loads them), triangles, materials."""
    nodes = document.get('nodes', [])
    parent = {}
    for index, node in enumerate(nodes):
        for child in node.get('children', []):
            parent[child] = index

    def world(index):
        matrix = node_matrix(nodes[index])
        while index in parent:
            index = parent[index]
            matrix = matmul(node_matrix(nodes[index]), matrix)
        return matrix

    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    triangles = 0
    for index, node in enumerate(nodes):
        if 'mesh' not in node:
            continue
        matrix = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]] if 'skin' in node else world(index)
        for primitive in document['meshes'][node['mesh']]['primitives']:
            if primitive.get('mode', 4) != 4:
                continue
            accessor = document['accessors'][primitive['attributes']['POSITION']]
            if 'indices' in primitive:
                triangles += document['accessors'][primitive['indices']]['count'] // 3
            else:
                triangles += accessor['count'] // 3
            mn, mx = accessor['min'], accessor['max']
            for cx in (mn[0], mx[0]):
                for cy in (mn[1], mx[1]):
                    for cz in (mn[2], mx[2]):
                        p = [sum(matrix[r][k] * (cx, cy, cz, 1)[k] for k in range(4)) for r in range(3)]
                        for k in range(3):
                            lo[k] = min(lo[k], p[k])
                            hi[k] = max(hi[k], p[k])
    return {
        'bounds': {'min': [round(v, 4) for v in lo], 'max': [round(v, 4) for v in hi]},
        'triangles': triangles,
        'materials': len(document.get('materials', [])),
        'textures': len(document.get('images', [])),
    }


def vec3(value, default):
    if value is None:
        return list(default)
    if isinstance(value, (int, float)):
        return [float(value)] * 3
    return [float(v) for v in value]


LEVEL_TYPES = ['town', 'cathedral', 'catacombs', 'caves', 'hell', 'nest', 'crypt']


def fmt(value: float) -> str:
    text = f'{value:.4f}'.rstrip('0').rstrip('.')
    return '0' if text in ('-0', '') else text


def placement_files(placement: dict, models: dict, install_dir: str, mesh_only: bool) -> dict:
    """Placement files (docs/design/static-mesh-placement.md, damnationx.placement v1) per level type.

    `models` maps a model id to its manifest entry (with file, scale, pivot, footprint, sha256). Rules whose model is
    not in `models` are left out. With `mesh_only` (a higher tier overriding meshes), only the [mesh.*] sections of
    the models the level type uses are written: the tier merge swaps the lower tier's mesh of the same id.
    """
    import math

    files = {}
    for level in LEVEL_TYPES:
        rules = [rule for rule in placement['placements'] if level in rule['levels'] and rule['model'] in models]
        if not rules:
            continue
        lines = ['; Generated by tools/build_3d_packs.py from tools/packs/3d/placement.json; do not edit.',
                 '[Placement]', 'format = damnationx.placement', 'version = 1', '']
        used = []
        for rule in rules:
            if rule['model'] not in used:
                used.append(rule['model'])
        for model_id in used:
            model = models[model_id]
            lines += [f'[mesh.{model_id}]', f"file = {install_dir}/{model['file']}", 'units = tiles', f"sha256 = {model['sha256']}", '']
        if mesh_only:
            files[f'{install_dir}/placements/{level}.ini'] = ('\n'.join(lines) + '\n').encode('utf-8')
            continue
        counts = {}
        for rule in rules:
            model = models[rule['model']]
            scale = [model['scale'][axis] * (rule.get('scale', [1, 1, 1])[axis] if isinstance(rule.get('scale', 1), list) else rule.get('scale', 1)) for axis in range(3)]
            yaw = float(rule.get('yaw', 0))
            counts[rule['model']] = counts.get(rule['model'], 0) + 1
            section_id = f"{rule['model']}-{counts[rule['model']]}"
            scale_text = fmt(scale[0]) if scale[0] == scale[1] == scale[2] else ','.join(fmt(v) for v in scale)
            anchor = rule.get('anchor', 'tile')
            if anchor == 'tile':
                ax, ay = rule['at']
                tx, ty = math.floor(ax), math.floor(ay)
                # The mesh's pivot (model space) lands on the anchor: offset by the scaled, turned pivot.
                pivot = [model['pivot'][axis] * scale[axis] for axis in range(3)]
                c, s_ = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
                px, pz = c * pivot[0] + s_ * pivot[2], -s_ * pivot[0] + c * pivot[2]
                lines += [f'[instance.{section_id}]', f"mesh = {rule['model']}", f'tile = {tx},{ty}',
                          f'offset = {fmt(ax - tx - px)},{fmt(-pivot[1])},{fmt(ay - ty - pz)}']
                if yaw:
                    lines.append(f'yaw = {fmt(yaw)}')
                lines.append(f'scale = {scale_text}')
                if rule.get('replaces') == 'town_house':
                    footprint = model['footprint']
                    rule_scale = rule.get('scale', [1, 1, 1]) if isinstance(rule.get('scale', 1), list) else [rule.get('scale', 1)] * 3
                    hx, hz = footprint[0] * rule_scale[0] / 2, footprint[1] * rule_scale[2] / 2
                    if round(yaw) % 180 == 90:
                        hx, hz = hz, hx
                    x0, x1 = math.ceil(ax - hx - 0.5), math.floor(ax + hx - 0.5)
                    y0, y1 = math.ceil(ay - hz - 0.5), math.floor(ay + hz - 0.5)
                    lines += [f'hide = {x0},{y0},{x1},{y1}', 'occluder = auto']
                else:
                    lines.append('occluder = none')
            else:
                lines += [f'[rule.{section_id}]', f"mesh = {rule['model']}"]
                if anchor == 'arch':
                    lines.append('arch = any')
                else:
                    lines.append('object = ' + ', '.join(rule['objects']))
                facing = rule.get('facing', 'passage' if anchor in ('door', 'arch') else 'fixed')
                if facing != 'fixed':
                    lines.append(f'facing = {facing}')
                if rule.get('show', 'always') != 'always':
                    lines.append(f"show = {rule['show']}")
                if yaw:
                    lines.append(f'yaw = {fmt(yaw)}')
                lines += [f'scale = {scale_text}', 'hide = self', 'occluder = none']
            if rule.get('note'):
                lines.insert(len(lines) - 1, f"; {rule['note']}")
            lines.append('')
        files[f'{install_dir}/placements/{level}.ini'] = ('\n'.join(lines) + '\n').encode('utf-8')
    return files


def build_pack(definition_path: pathlib.Path, source_root: pathlib.Path, max_texture: int, version: str, hd_art: pathlib.Path | None):
    definition = json.loads(definition_path.read_text(encoding='utf-8'))
    tier = definition['tier']
    install_dir = definition['install_dir'].strip('/')
    model_root = source_root / definition['source_dir']
    files = {}  # zip path -> bytes
    manifest_models = []
    missing = []

    for entry in definition.get('models', []):
        source = model_root / entry['source']
        if not source.exists():
            missing.append(str(source))
            continue
        document, binary = read_glb(source.read_bytes())
        binary = reencode_textures(document, binary, max_texture)
        info = inspect(document)
        data = write_glb(document, binary)
        lo, hi = info['bounds']['min'], info['bounds']['max']
        scale = vec3(entry.get('scale'), (1.0, 1.0, 1.0))
        pivot = vec3(entry.get('pivot'), ((lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2))
        footprint = entry.get('footprint') or [round((hi[0] - lo[0]) * scale[0], 3), round((hi[2] - lo[2]) * scale[2], 3)]
        relative = f"models/{entry['id']}.glb"
        files[f'{install_dir}/{relative}'] = data
        manifest_models.append({
            'id': entry['id'],
            'file': relative,
            'tier': tier,
            'source': entry['source'],
            'scale': scale,
            'pivot': [round(v, 4) for v in pivot],
            'yaw': entry.get('yaw', 0),
            'footprint': footprint,
            'height': round((hi[1] - pivot[1]) * scale[1], 3),
            'bounds': info['bounds'],
            'triangles': info['triangles'],
            'materials': info['materials'],
            'textures': info['textures'],
            'bytes': len(data),
            'sha256': hashlib.sha256(data).hexdigest(),
        })
        print(f"  {entry['id']:22} {len(data) / 1e6:6.2f} MB  {info['triangles']:6} tris  {info['textures']:3} textures  <- {entry['source']}")

    characters = []
    for entry in definition.get('characters', []):
        source = source_root / definition.get('character_source_dir', '') / entry['source']
        if not source.exists():
            missing.append(str(source))
            continue
        original = source.read_bytes()
        document, binary = read_glb(original)
        install = entry['install']
        files[install] = original
        variants = {}
        for suffix, max_texture_variant in entry.get('variants', {}).items():
            variant_document, variant_binary = read_glb(original)
            variant_binary = reencode_textures(variant_document, variant_binary, max_texture_variant)
            data = write_glb(variant_document, variant_binary)
            variant_path = install[:-len('.glb')] + suffix + '.glb'
            files[variant_path] = data
            variants[variant_path] = {'max_texture': max_texture_variant, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        skin = document.get('skins', [{}])[0]
        characters.append({
            'class': entry['class'],
            'version': entry.get('version', ''),
            'file': install,
            'source': entry['source'],
            'bytes': len(original),
            'sha256': hashlib.sha256(original).hexdigest(),
            'variants': variants,
            'joints': len(skin.get('joints', [])),
            'meshes': sorted(node['name'] for node in document.get('nodes', []) if 'mesh' in node),
            'clips': sorted(animation.get('name', '') for animation in document.get('animations', [])),
            'textures': len(document.get('images', [])),
        })
        print(f"  {install:40} {len(original) / 1e6:6.2f} MB  {len(characters[-1]['meshes'])} meshes  {len(characters[-1]['clips'])} clips"
              + ''.join(f"  + {path.rsplit('/', 1)[1]} {info['bytes'] / 1e6:.1f} MB" for path, info in variants.items()))
    if characters:
        files['hd/3d/characters.json'] = (json.dumps({'schema': 'damnationx.3d-characters/1', 'version': version, 'characters': characters}, indent=2) + '\n').encode('utf-8')

    for name, source in definition.get('files', {}).items():
        files[f'{install_dir}/{name}'] = (PACK_SOURCES / source).read_bytes()

    for extra in definition.get('extra', []):
        # relative_to assets: the assets folder the downloads live in (assets/downloaded/..)
        base = source_root.parent if extra.get('relative_to') == 'assets' else source_root
        source = base / extra['source']
        if source.exists():
            files[extra['install']] = source.read_bytes()
            print(f"  {extra['install']:40} {source.stat().st_size / 1e6:6.2f} MB  <- {source}")
        elif not extra.get('optional'):
            missing.append(str(source))

    if hd_art is not None:
        for path in sorted(hd_art.rglob('*')):
            if path.is_file() and path.name != '.DS_Store':
                files[f'hd/{path.relative_to(hd_art).as_posix()}'] = path.read_bytes()

    placement_source = definition.get('placement')
    if placement_source and manifest_models:
        placement = json.loads((PACK_SOURCES / placement_source).read_text(encoding='utf-8'))
        by_id = {model['id']: model for model in manifest_models}
        for path, data in placement_files(placement, by_id, install_dir, definition.get('placement_meshes_only', False)).items():
            files[path] = data
            print(f'  {path}')

    manifest = {
        'schema': 'damnationx.3d-pack/1',
        'pack': definition['pack'],
        'tier': tier,
        'title': definition['title'],
        'version': version,
        'attribution': definition['attribution'],
        'license': definition['license'],
        'max_texture': max_texture,
        'models': manifest_models,
    }
    files[f'{install_dir}/manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
    return definition['pack'], files, missing


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--pack', choices=['free', 'hd', 'all'], default='all')
    parser.add_argument('--source', type=pathlib.Path, default=DEFAULT_SOURCE, help='downloaded art (default assets/downloaded)')
    parser.add_argument('--out', type=pathlib.Path, default=DEFAULT_OUT, help='output folder (default assets/packs)')
    parser.add_argument('--max-texture', type=int, default=1024, help='longest texture side in the packs (default 1024)')
    parser.add_argument('--version', default=datetime.date.today().strftime('%Y%m%d'))
    parser.add_argument('--install', type=pathlib.Path, action='append', default=[],
                        help='also unpack into this folder (it gets hd/3d/...); may be repeated')
    parser.add_argument('--hd-art', type=pathlib.Path, help='HD art folder merged into the HD zip as hd/')
    parser.add_argument('--allow-missing', action='store_true', help='build even when listed models are missing')
    args = parser.parse_args()

    try:
        import PIL  # noqa: F401
    except ImportError:
        print('Pillow is required: pip install pillow', file=sys.stderr)
        return 2

    out = args.out.resolve()
    if ROOT in out.parents or out == ROOT:
        relative = out.relative_to(ROOT).as_posix() + '/'
        if not (relative.startswith('assets/packs/') or relative.startswith('assets/downloaded/') or relative.startswith('build')):
            print(f'refusing to write packs to {relative}: it is not gitignored (use assets/packs/)', file=sys.stderr)
            return 2
    out.mkdir(parents=True, exist_ok=True)

    packs = ['free', 'hd'] if args.pack == 'all' else [args.pack]
    sums = []
    for name in packs:
        print(f'{name} pack:')
        pack_name, files, missing = build_pack(PACK_SOURCES / f'{name}.pack.json', args.source.resolve(), args.max_texture, args.version,
                                               args.hd_art if name == 'hd' else None)
        if missing:
            print('  missing sources:\n    ' + '\n    '.join(missing), file=sys.stderr)
            if not args.allow_missing:
                return 1
        archive = out / f'{pack_name}-{args.version}.zip'
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            for path in sorted(files):
                info = zipfile.ZipInfo(path, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                zf.writestr(info, files[path])
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        sums.append(f'{digest}  {archive.name}')
        print(f'  -> {archive.relative_to(ROOT) if ROOT in archive.parents else archive} ({archive.stat().st_size / 1e6:.1f} MB, {len(files)} files)')
        for target in args.install:
            for path, data in files.items():
                destination = target / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
            print(f'  installed into {target}')
    (out / 'SHA256SUMS').write_text('\n'.join(sums) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
