"""Pack merging uses synthetic models and images, never distributed game art."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image


SOURCE = Path(__file__).resolve().parents[1] / 'build_3d_packs.py'
SPEC = importlib.util.spec_from_file_location('build_3d_packs', SOURCE)
pack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pack)


class PackMergeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.art = self.root / 'installed hd'
        self.art.mkdir()

    def write(self, relative, data=b'synthetic bytes'):
        target = self.art / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def link(self, target, link):
        try:
            link.symlink_to(target, target_is_directory=target.is_dir())
        except (OSError, NotImplementedError) as error:
            self.skipTest(f'Symlinks unavailable: {error}')

    def files(self):
        return {relative.as_posix(): path.read_bytes() for relative, path in pack.hd_art_files(self.art)}

    def definition(self):
        source = self.root / 'model sources'
        source.mkdir()
        document = {
            'asset': {'version': '2.0', 'generator': 'synthetic current model'},
            'scenes': [{'nodes': [0]}], 'scene': 0,
            'nodes': [{'name': 'triangle', 'mesh': 0}],
            'meshes': [{'primitives': [{'attributes': {'POSITION': 0}}]}],
            'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3,
                           'type': 'VEC3', 'min': [0, 0, 0], 'max': [1, 1, 0]}],
            'bufferViews': [{'buffer': 0, 'byteLength': 36}],
            'buffers': [{'byteLength': 36}],
        }
        glb = pack.write_glb(document, struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0))
        (source / 'prop.glb').write_bytes(glb)
        (source / 'hero.glb').write_bytes(glb)
        definition = {
            'tier': 'hd', 'install_dir': 'hd/3d/hd', 'source_dir': '.',
            'pack': 'synthetic-hd', 'title': 'Synthetic HD', 'attribution': 'test', 'license': 'test',
            'models': [{'id': 'crate', 'source': 'prop.glb'}],
            'characters': [{'class': 'warrior', 'source': 'hero.glb',
                            'install': 'hd/3d/warrior/warrior.glb', 'variants': {'_preview': 32}}],
            'placement': 'placement.json', 'placement_meshes_only': True,
        }
        definition_path = self.root / 'hd.pack.json'
        definition_path.write_text(json.dumps(definition))
        (self.root / 'placement.json').write_text(json.dumps({'placements': [{
            'model': 'crate', 'levels': ['cathedral'], 'anchor': 'object', 'objects': ['OBJ_BARREL'],
        }]}))
        return definition_path, source

    def test_current_models_characters_and_placements_survive_old_combined_art(self):
        definition, source = self.definition()
        self.write('sprites.txt', b'current 2D sprite index')
        image = io.BytesIO()
        Image.new('RGBA', (2, 2), (255, 0, 0, 255)).save(image, format='PNG')
        self.write('sprites/test.png', image.getvalue())
        stale_paths = [
            '3d/hd/models/crate.glb', '3d/hd/placements/cathedral.ini',
            '3d/hd/placements/catacombs.ini', '3d/free/placements/cathedral.ini',
            '3d/free/manifest.json', '3d/warrior/warrior.glb',
            '3d/warrior/warrior_preview.glb', '3d/characters.json',
        ]
        for path in stale_paths:
            self.write(path, b'obsolete model or chest placement')
        with patch.object(pack, 'PACK_SOURCES', self.root), contextlib.redirect_stdout(io.StringIO()):
            _, files, missing = pack.build_pack(definition, source, 1024, 'test-version', self.art)
        self.assertEqual(missing, [])
        self.assertEqual(files['hd/sprites.txt'], b'current 2D sprite index')
        self.assertEqual(files['hd/sprites/test.png'], image.getvalue())
        self.assertNotIn('hd/3d/free/placements/cathedral.ini', files)
        self.assertNotIn('hd/3d/free/manifest.json', files)
        self.assertNotIn('hd/3d/hd/placements/catacombs.ini', files)
        self.assertIn(b'[mesh.crate]', files['hd/3d/hd/placements/cathedral.ini'])
        self.assertNotIn(b'obsolete', files['hd/3d/hd/placements/cathedral.ini'])
        manifest = json.loads(files['hd/3d/hd/manifest.json'])
        for model in manifest['models']:
            data = files['hd/3d/hd/' + model['file']]
            self.assertEqual(model['sha256'], hashlib.sha256(data).hexdigest())
            self.assertEqual(model['bytes'], len(data))
        characters = json.loads(files['hd/3d/characters.json'])
        self.assertEqual(characters['version'], 'test-version')
        for character in characters['characters']:
            self.assertEqual(character['sha256'], hashlib.sha256(files[character['file']]).hexdigest())
            for name, variant in character['variants'].items():
                self.assertEqual(variant['sha256'], hashlib.sha256(files[name]).hexdigest())

    def test_linked_directories_and_files_are_materialized_under_their_logical_names(self):
        external = self.root / 'source sprites'
        (external / 'nested').mkdir(parents=True)
        (external / 'nested/frame.png').write_bytes(b'linked sprite')
        self.link(external, self.art / 'sprites')
        self.link(external, self.art / 'second-reference')
        self.link(external / 'nested/frame.png', self.art / 'item.png')
        self.write('sprites.txt', b'sprites/nested/frame.png')
        files = self.files()
        self.assertEqual(files['sprites/nested/frame.png'], b'linked sprite')
        self.assertEqual(files['second-reference/nested/frame.png'], b'linked sprite')
        self.assertEqual(files['item.png'], b'linked sprite')

    def test_cyclic_directory_links_fail_with_an_actionable_error(self):
        sprites = self.art / 'sprites'
        sprites.mkdir()
        self.link(self.art, sprites / 'back-to-root')
        with self.assertRaisesRegex(ValueError, 'directory link cycle.*back-to-root'):
            self.files()

    def test_broken_links_and_missing_source_fail_instead_of_creating_incomplete_packs(self):
        self.link(self.root / 'missing.png', self.art / 'missing.png')
        with self.assertRaisesRegex(ValueError, 'broken link.*missing.png'):
            self.files()
        with self.assertRaisesRegex(ValueError, 'Cannot read HD art source'):
            list(pack.hd_art_files(self.root / 'missing source'))

    def test_old_3d_tree_is_excluded_case_insensitively_without_following_its_links(self):
        old_3d = self.art / '3D'
        old_3d.mkdir()
        self.link(old_3d, old_3d / 'cycle')
        self.write('sprites.txt', b'2D art')
        self.write('.DS_Store', b'ignored')
        self.assertEqual(self.files(), {'sprites.txt': b'2D art'})

    def test_combined_download_keeps_both_tiers_2d_bytes_and_component_licenses(self):
        image = b'existing WebP export bytes'
        self.write('sprites.txt', b'sprites/test.webp')
        self.write('ui.txt', b'ui/test.webp')
        self.write('sprites/test.webp', image)
        self.write('3d/free/placements/cathedral.ini', b'old chest rule')
        self.write('3d/hd/models/prop.glb', b'old prop')
        calls = []

        def component(definition, source, size, version, art):
            tier = definition.stem.split('.')[0]
            calls.append((tier, art))
            root = f'hd/3d/{tier}'
            files = {f'{root}/manifest.json': json.dumps({
                'attribution': f'{tier} author', 'license': f'{tier} terms'}).encode(),
                f'{root}/models/prop.glb': tier.encode(),
                f'{root}/placements/cathedral.ini': b'current placement'}
            if tier == 'free':
                files[f'{root}/NOTICE.md'] = b'full original notice'
                files[f'{root}/LICENSE.md'] = b'full original license'
            return tier, files, []

        with patch.object(pack, 'build_pack', side_effect=component):
            name, files, missing = pack.build_combined(self.root, 1024, 'test', self.art)
        self.assertEqual(name, 'damnationx-art-pack')
        self.assertEqual(missing, [])
        self.assertEqual(calls, [('free', None), ('hd', None)])
        self.assertEqual(files['hd/sprites/test.webp'], image)
        self.assertEqual(files['hd/3d/free/NOTICE.md'], b'full original notice')
        self.assertEqual(files['hd/3d/free/LICENSE.md'], b'full original license')
        self.assertEqual(files['hd/3d/hd/models/prop.glb'], b'hd')
        self.assertEqual(files['hd/3d/free/placements/cathedral.ini'], b'current placement')
        metadata = json.loads(files['ART-PACK.json'])
        self.assertEqual(metadata['version'], 'test')
        self.assertEqual(metadata['d2r_imported_data'], 'excluded')
        self.assertEqual([c['tier'] for c in metadata['components']], ['free', 'hd'])
        self.assertIn(b'Import ZIP', files['README.txt'])

    def test_combined_download_refuses_incomplete_2d_art(self):
        def empty_component(*args):
            return 'component', {}, []
        with patch.object(pack, 'build_pack', side_effect=empty_component):
            with self.assertRaisesRegex(ValueError, 'requires hd/sprites.txt'):
                pack.build_combined(self.root, 1024, 'test', self.art)

    def test_default_cli_writes_one_combined_archive_and_checksum(self):
        output = self.root / 'output'
        payload = {'README.txt': b'one complete download', 'hd/sprites.txt': b'index'}
        argv = ['build_3d_packs.py', '--hd-art', str(self.art), '--out', str(output), '--version', 'test']
        with patch.object(pack.sys, 'argv', argv), \
                patch.object(pack, 'build_combined', return_value=('damnationx-art-pack', payload, [])) as combined, \
                patch.object(pack, 'build_pack') as separate, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pack.main(), 0)
        combined.assert_called_once()
        separate.assert_not_called()
        archives = list(output.glob('*.zip'))
        self.assertEqual([p.name for p in archives], ['damnationx-art-pack-test.zip'])
        self.assertEqual((output / 'SHA256SUMS').read_text(),
                         f'{pack.file_sha256(archives[0])}  {archives[0].name}\n')


if __name__ == '__main__':
    unittest.main()
