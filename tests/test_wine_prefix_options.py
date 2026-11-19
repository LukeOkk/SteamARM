import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENTRY = REPO / "build/steamarm-wine-prefix-options"

REG = '''WINE REGISTRY Version 2
;; All keys relative to \\User\\S-1-5-21-0-0-0-1000

#arch=win64

[Software\\Wine\\Direct3D] 1700000000
#time=1d0
"csmt"=dword:00000001

[Software\\Wine\\X11 Driver] 1700000000
#time=1d0
"Decorated"="N"
'''


class EmulateModeset(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not ENTRY.is_file():
            subprocess.run(["make", "wine-prefix-options"], cwd=REPO, check=True)

    def invoke(self, state, *args):
        env = dict(os.environ, STEAMARM_STATE=str(state))
        return subprocess.run(
            [str(ENTRY), "emulate-modeset", *args],
            cwd=REPO,
            env=env,
            text=True,
            capture_output=True,
            check=True,
        )

    def test_add_into_existing_section_and_remove(self):
        with tempfile.TemporaryDirectory() as d:
            state = Path(d)
            reg = state / "user.reg"
            reg.write_text(REG)
            self.assertIn(str(reg), self.invoke(state, "on", str(reg)).stdout)
            on = reg.read_text()
            self.assertIn('#time=1d0\n"EmulateModeset"="Y"\n"Decorated"="N"', on)
            self.assertEqual(self.invoke(state, "on", str(reg)).stdout, "")
            self.invoke(state, "off", str(reg))
            self.assertEqual(reg.read_text(), REG)
            self.assertEqual(self.invoke(state, "off", str(reg)).stdout, "")

    def test_new_section(self):
        with tempfile.TemporaryDirectory() as d:
            state = Path(d)
            reg = state / "user.reg"
            base = REG.replace(
                '[Software\\Wine\\X11 Driver] 1700000000\n#time=1d0\n"Decorated"="N"\n',
                "",
            )
            reg.write_text(base)
            self.invoke(state, "on", str(reg))
            self.assertRegex(
                reg.read_text(),
                re.compile(
                    r'\[Software\\Wine\\X11 Driver\] \d+\n"EmulateModeset"="Y"\n$'
                ),
            )
            self.assertIn("[Software\\Wine\\Direct3D]", reg.read_text())

    def test_other_value_replaced(self):
        with tempfile.TemporaryDirectory() as d:
            state = Path(d)
            reg = state / "user.reg"
            reg.write_text(REG.replace('"Decorated"="N"', '"EmulateModeset"="N"\n"Decorated"="N"'))
            self.invoke(state, "on", str(reg))
            self.assertIn('"EmulateModeset"="Y"', reg.read_text())
            self.invoke(state, "off", str(reg))
            self.assertNotIn("EmulateModeset", reg.read_text())

    def test_default_files(self):
        with tempfile.TemporaryDirectory() as d:
            state = Path(d)
            relatives = (
                "steamroot/tmp/fexhome/.local/share/Steam/steamapps/compatdata/10/pfx/user.reg",
                "arm64root/tmp/armhome/.local/share/Steam/steamapps/compatdata/20/pfx/user.reg",
                "steamroot/tmp/fexhome/.steamarm/prefixes/app/pfx/user.reg",
            )
            files = [state / relative for relative in relatives]
            for path in files:
                path.parent.mkdir(parents=True)
                path.write_text(REG)
            self.invoke(state, "on")
            self.assertTrue(all('"EmulateModeset"="Y"' in p.read_text() for p in files))
            self.invoke(state, "off")
            self.assertTrue(all(p.read_text() == REG for p in files))


if __name__ == "__main__":
    unittest.main()
