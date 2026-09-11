import importlib.util
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("machine_dsl", ROOT / "tools/machine-dsl/86box_machine.py")
DSL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DSL
SPEC.loader.exec_module(DSL)


class ToolkitTests(unittest.TestCase):
    def parse(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "test.86m"
            path.write_text(text, encoding="utf-8")
            return DSL.parse_file(path)

    def test_minimal_machine(self):
        document = self.parse('schema 1\nmachine demo {\n name = "Demo"\n}\n')
        self.assertEqual(document.schema, 1)
        self.assertEqual(DSL.declaration_identity(document.declarations[0]), ("machine", "demo"))

    def test_round_trip_is_stable(self):
        document = self.parse('schema 1\nmachine demo{\nname="Demo"\n}\n')
        once = DSL.format_document(document)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "test.86m"
            path.write_text(once, encoding="utf-8")
            twice = DSL.format_document(DSL.parse_file(path))
        self.assertEqual(once, twice)

    def test_rejects_parent_path(self):
        document = self.parse('schema 1\nmachine demo {\n name = "Demo"\n file = "../bad.rom"\n}\n')
        errors = DSL.validate([document])
        self.assertTrue(any("firmware path" in error for error in errors))

    def test_repository_examples(self):
        documents = DSL.load([str(ROOT / "examples/machines")])
        self.assertEqual([], DSL.validate(documents))

    def test_runtime_descriptions(self):
        documents = DSL.load([str(ROOT / "src/machine/dsl")])
        self.assertEqual([], DSL.validate(documents))


if __name__ == "__main__":
    unittest.main()
