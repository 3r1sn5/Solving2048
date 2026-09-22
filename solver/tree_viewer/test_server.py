import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("server.py")
SPEC = importlib.util.spec_from_file_location("tree_viewer_server", MODULE_PATH)
viewer = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(viewer)


class TreeViewerIndexTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temporary.name)
        cls.sgf = cls.directory / "proof.sgf"
        cls.rz = cls.directory / "proof.rz"
        cls.index = cls.directory / "proof.tree-index"
        cls.sgf.write_text(
            "(;FF[4]CA[UTF-8]SZ[4]AP[Solving2048:1]"
            ";W[4];W[5]C[solver_status: WIN\n"
            "solved_by = BACKPROPAGATION\n"
            "rzone_certificate_id = 2\n"
            "rzone_transform = 0\n"
            "selected_action = 3\n"
            "winning_tile = 4\n"
            "node_limit = 100\n"
            "transposition_table = disabled\n"
            "rzone_mode = lookup\n]"
            "(;B[3]C[solver_status: WIN\n"
            "solved_by = TERMINAL\n"
            "rzone_certificate_id = 1\n"
            "rzone_transform = 0\n])"
            "(;B[2]C[solver_status: WIN\n"
            "solved_by = TERMINAL\n"
            "rzone_certificate_id = 1\n"
            "rzone_transform = 0\n]))\n",
            encoding="utf-8",
        )
        header = bytearray(128)
        header[:8] = b"S2048RZ\0"
        struct.pack_into("<HHHH", header, 8, 1, 128, 32, 0)
        struct.pack_into("<I", header, 32, 4)
        struct.pack_into("<Q", header, 48, 2)
        terminal = viewer.RZoneFile.RECORD.pack(2, 0, 1, 1, 1, 2, 1, 0, 0, 0)
        parent = viewer.RZoneFile.RECORD.pack(0x11, 1, 2, 3, 2, 1, 1, 3, 0, 0)
        cls.rz.write_bytes(header + terminal + parent)
        viewer.build_index(cls.sgf, cls.index, cls.rz, None)
        cls.store = viewer.NodeStore(cls.index)

    @classmethod
    def tearDownClass(cls):
        cls.store.close()
        cls.temporary.cleanup()

    def test_stream_index_preserves_tree_and_boards(self):
        self.assertEqual(self.store.metadata["node_count"], 3)
        root = self.store.node(1)
        terminal = self.store.node(2)
        self.assertEqual(root["board"], "0x0000000000000011")
        self.assertEqual(root["children"], 2)
        self.assertEqual(root["subtree_end"], 3)
        self.assertEqual(terminal["parent"], 1)
        self.assertEqual(terminal["board"], "0x0000000000000002")
        self.assertEqual(terminal["solved_by"], "TERMINAL")

    def test_range_and_rzone_overlay_are_random_access(self):
        payload = self.store.range(1, 2, 0, 2, 100)
        self.assertEqual(payload["mode"], "exact")
        self.assertEqual([node["id"] for node in payload["nodes"]], [1, 2])
        certificate = self.store.rzone.certificate(1)
        self.assertEqual(certificate["display_mask"], "0x0001")
        self.assertEqual(certificate["display_masked_board"], "0x0000000000000002")

    def test_metadata_is_json_serializable(self):
        json.dumps(self.store.metadata)

    def test_root_trajectory_follows_first_children_to_leaf(self):
        trajectory = self.store.root_trajectory()
        self.assertEqual([node["id"] for node in trajectory], [1, 2])
        self.assertEqual(trajectory[-1]["solved_by"], "TERMINAL")

    def test_navigation_exposes_parent_child_and_siblings(self):
        root = self.store.node(1, include_board=False)
        child = self.store.node(2, include_board=False)
        self.assertEqual(root["first_child"], 2)
        self.assertEqual(child["parent"], 1)
        self.assertEqual(self.store.navigation(2), {
            "previous_sibling": 0,
            "next_sibling": 3,
        })
        self.assertEqual(self.store.navigation(3), {
            "previous_sibling": 2,
            "next_sibling": 0,
        })


if __name__ == "__main__":
    unittest.main()
