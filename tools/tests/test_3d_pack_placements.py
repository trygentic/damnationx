"""Source-free checks for gameplay-state continuity in generated 3D placements."""
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("placement_pack_builder", ROOT / "tools/build_3d_packs.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class PackPlacementTests(unittest.TestCase):
    def test_both_tiers_leave_stateful_chests_to_the_native_or_d2r_renderer(self):
        placement = json.loads((ROOT / "tools/packs/3d/placement.json").read_text())
        chest_ids = {"OBJ_CHEST1", "OBJ_CHEST2", "OBJ_CHEST3", "OBJ_TCHEST1", "OBJ_TCHEST2", "OBJ_TCHEST3"}
        for tier in ("free", "hd"):
            definition = json.loads((ROOT / f"tools/packs/3d/{tier}.pack.json").read_text())
            models = {
                item["id"]: {
                    "file": f"models/{item['id']}.glb", "sha256": "0" * 64,
                    "scale": [1, 1, 1], "pivot": [0, 0, 0], "footprint": [1, 1],
                }
                for item in definition["models"]
            }
            # Keep the assets available for future authored open variants.
            self.assertIn("chest_wooden", models)
            generated = builder.placement_files(
                placement, models, f"hd/3d/{tier}", definition.get("placement_meshes_only", False))
            cathedral = generated[f"hd/3d/{tier}/placements/cathedral.ini"].decode()
            for chest_id in chest_ids:
                self.assertNotIn(chest_id, cathedral)
            self.assertNotIn("chest_", cathedral)
            self.assertIn("[mesh.brazier_metal]", cathedral)
            self.assertIn("[mesh.door_arch_wooden]", cathedral)
            if tier == "free":
                self.assertIn("object = OBJ_L1LIGHT", cathedral)
                self.assertIn("object = OBJ_L1LDOOR, OBJ_L1RDOOR", cathedral)
                self.assertIn("arch = any", cathedral)
            else:
                self.assertNotIn("[rule.", cathedral)


if __name__ == "__main__":
    unittest.main()
