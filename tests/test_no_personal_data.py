"""Nothing personal in the repository: the files git tracks must not carry a
home directory, an e-mail address, a private network address or a time zone
of whoever ran the measurements. Benchmarks and logs are pasted from real
runs, so this guards what they bring along (a home router's address was
found in a stage log, benchmarks/stage44)."""
import re
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

PATTERNS = {
    "home directory": re.compile(r"/Users/(?!runner\b|Shared\b|you\b|me\b|<|\$|\{|USER\b|name\b|username\b)[A-Za-z][\w.-]*"),
    "e-mail address": re.compile(r"\b[\w.+-]+@(?!users\.noreply\.github\.com|noreply\.github\.com|anthropic\.com|example\.(?:com|org))"
                                 r"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.(?:com|net|org|me|io|dev|uy|ar|es|mx|br)\b"),
    "private network address": re.compile(r"\b192\.168\.\d{1,3}\.\d{1,3}\b"),
    "time zone": re.compile(r"\b(?:America|Europe|Asia|Africa|Australia)/[A-Z][a-z]+(?:_[A-Z][a-z]+)?\b"),
}

# Matches that are not anyone's here: the Steam Frame image's own zone
# (Valve's build, scripts/mkframeroot.sh) and the settings tests' example.
ALLOWED = {"America/Los_Angeles", "Europe/Berlin"}

# Paths whose text is someone else's (licenses, upstream sources, patches of
# upstream code) or that name such patterns on purpose (this test).
SKIP = ("third_party/", "tests/test_no_personal_data.py", "patches/")


def tracked():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, capture_output=True, check=True).stdout
    return [p for p in out.decode().split("\0") if p and not p.startswith(SKIP)]


class NoPersonalData(unittest.TestCase):
    def test_tracked_files(self):
        found = []
        for rel in tracked():
            path = REPO / rel
            try:
                data = path.read_bytes()
            except OSError:
                continue
            if b"\0" in data[:4096]:
                continue
            text = data.decode("utf-8", "replace")
            for what, rx in PATTERNS.items():
                for m in rx.finditer(text):
                    if m.group(0) in ALLOWED:
                        continue
                    line = text.count("\n", 0, m.start()) + 1
                    found.append("%s:%d: %s %r" % (rel, line, what, m.group(0)))
        self.assertEqual(found, [], "\n" + "\n".join(found[:40]))

    def test_patterns(self):
        self.assertTrue(PATTERNS["home directory"].search("cd /Users/alice/x"))
        self.assertFalse(PATTERNS["home directory"].search("/Users/runner/work and /Users/Shared"))
        self.assertTrue(PATTERNS["e-mail address"].search("mail bob.smith@gmail.com"))
        self.assertFalse(PATTERNS["e-mail address"].search("1+x@users.noreply.github.com noreply@anthropic.com"))
        self.assertTrue(PATTERNS["private network address"].search("dns 192.168.0.1"))
        self.assertTrue(PATTERNS["time zone"].search("TZ=Europe/Madrid"))


if __name__ == "__main__":
    unittest.main()
