"""The docs against the records and the source they cite.

Run: python3 -m unittest tests/test_docs_records.py   (no guest, no network)

Each check guards a defect a review found in the docs: a stage record missing
from the benchmark index, a doc citing a record that does not exist, the x18
refusal table drifting from runtime/x18.c, launcher entries the user guide
does not name, release notes missing for the launcher's version, and personal
paths in committed text.
"""
import json
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[1]


def current_docs():
    """README and every doc except docs/history (kept as written)."""
    docs = [REPO / "README.md", REPO / "benchmarks/README.md"]
    docs += sorted(p for p in (REPO / "docs").rglob("*.md")
                   if "history" not in p.relative_to(REPO / "docs").parts)
    return docs


class BenchmarkIndexTests(unittest.TestCase):
    def test_every_stage_record_is_indexed(self):
        index = (REPO / "benchmarks/README.md").read_text()
        table = index.split("## Stage index", 1)[1].split("\n## ", 1)[0]
        named = set(re.findall(r"`(stage[^`]+\.txt)`", table))
        records = {p.name for p in (REPO / "benchmarks").glob("stage*.txt")}
        self.assertEqual(sorted(records - named), [],
                         "stage records with no row in benchmarks/README.md's stage index")

    def test_cited_records_exist(self):
        missing = []
        for doc in current_docs():
            for name in re.findall(r"benchmarks/(stage[A-Za-z0-9_.-]+\.txt)", doc.read_text()):
                if not (REPO / "benchmarks" / name).is_file():
                    missing.append(f"{doc.relative_to(REPO)}: {name}")
        self.assertEqual(missing, [])


class X18DocTests(unittest.TestCase):
    def test_refusal_table_matches_planner(self):
        source = (REPO / "runtime/x18.c").read_text()
        planner = set(re.findall(r'reject\(p, "([^"]+)"\)', source))
        self.assertTrue(planner, "no reject() reasons found in runtime/x18.c")
        doc = (REPO / "docs/X18_VIRTUALIZATION.md").read_text()
        section = doc.split("## What the planner refuses", 1)[1].split("\n## ", 1)[0]
        table = set(re.findall(r"^\| `([^`]+)` \|", section, re.M))
        self.assertEqual(table, planner,
                         "docs/X18_VIRTUALIZATION.md 'What the planner refuses' vs runtime/x18.c")


class LauncherDocTests(unittest.TestCase):
    def test_builtin_entries_are_named_in_the_guides(self):
        apps = json.loads((REPO / "scripts/builtin-apps.json").read_text())
        # Markdown wraps lines anywhere: compare with whitespace collapsed.
        guides = {path: " ".join((REPO / path).read_text().split())
                  for path in ("docs/USAGE.md", "docs/es/GUIA.md")}
        for app in apps:
            for path, text in guides.items():
                with self.subTest(app=app["id"], guide=path):
                    self.assertTrue(app["name"] in text, f"{path} does not name {app['name']!r}")

    def test_release_notes_for_the_launcher_version(self):
        plist = (REPO / "launcher/Info.plist.in").read_text()
        match = re.search(r"<key>CFBundleShortVersionString</key>\s*<string>([^<]+)</string>", plist)
        self.assertIsNotNone(match, "no CFBundleShortVersionString in launcher/Info.plist.in")
        self.assertTrue((REPO / "docs/releases" / f"v{match.group(1)}.md").is_file(),
                        f"docs/releases/v{match.group(1)}.md is missing")


class PrivacyTests(unittest.TestCase):
    def test_no_home_paths_in_docs_or_records(self):
        home = re.compile(r"/Users/(?!Shared\b)[A-Za-z0-9._-]+")
        texts = current_docs() + sorted((REPO / "benchmarks").glob("*.txt"))
        found = [f"{p.relative_to(REPO)}: {m}" for p in texts for m in home.findall(p.read_text())]
        self.assertEqual(found, [])


if __name__ == "__main__":
    unittest.main()
