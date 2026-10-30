"""Experiment provenance must notice changed artwork, ROMFS, headers and libraries."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("stage1_builder", Path(__file__).resolve().parents[1] / "tools/build-stage1-variants.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

class BuildInputsTests(unittest.TestCase):
    def test_source_tracks_all_artifact_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            names = ["src/a.cpp", "assets/icon.jpg", "romfs/a.svg", "licenses/COPYING", "README.md", "docs/THIRD-PARTY.md", "tools/recipe.py"]
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"original")
            listing = b"\0".join(name.encode() for name in names) + b"\0"
            with patch.object(builder, "ROOT", root), patch.object(builder.subprocess, "check_output", return_value=listing):
                for name in names:
                    before = builder.source_digest()
                    (root / name).write_bytes(b"changed")
                    self.assertNotEqual(builder.source_digest(), before, name)
                    (root / name).write_bytes(b"original")

    def test_dependencies_track_binaries_headers_and_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / "sdk"
            roots = [sdk / "devkitA64", sdk / "libnx", sdk / "portlibs/switch", sdk / "tools",
                     root / "third_party/rtc/switch", root / "third_party/moonlight/switch", root / "third_party/ffmpeg-diag/switch"]
            for directory in roots:
                directory.mkdir(parents=True)
                (directory / "fixture.a").write_bytes(b"archive")
                (directory / "fixture.h").write_bytes(b"header")
            with patch.object(builder, "ROOT", root), patch.dict(builder.os.environ, {"DEVKITPRO": str(sdk), "DEVKITA64": str(sdk / "devkitA64")}), patch.object(builder.subprocess, "check_output", return_value="compiler version"):
                for directory in roots:
                    for name in ("fixture.a", "fixture.h"):
                        before = builder.input_digest(builder.dependency_inputs())
                        path = directory / name
                        previous = path.read_bytes()
                        path.write_bytes(b"modified")
                        self.assertNotEqual(builder.input_digest(builder.dependency_inputs()), before, str(path))
                        path.write_bytes(previous)

if __name__ == "__main__":
    unittest.main()
