import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("wpo", REPO / "scripts/wine-prefix-options.py")
wpo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wpo)

REG = '''WINE REGISTRY Version 2
;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000

#arch=win64

[Software\\\\Wine\\\\Direct3D] 1700000000
#time=1d0
"csmt"=dword:00000001

[Software\\\\Wine\\\\X11 Driver] 1700000000
#time=1d0
"Decorated"="N"
'''


class EmulateModeset(unittest.TestCase):
    def test_add_into_existing_section_and_remove(self):
        on = wpo.set_value(REG, True)
        self.assertIn('#time=1d0\n"EmulateModeset"="Y"\n"Decorated"="N"', on)
        self.assertEqual(wpo.set_value(on, True), on, "idempotent")
        off = wpo.set_value(on, False)
        self.assertEqual(off, REG)
        self.assertEqual(wpo.set_value(REG, False), REG)

    def test_new_section(self):
        base = REG.replace('[Software\\\\Wine\\\\X11 Driver] 1700000000\n#time=1d0\n"Decorated"="N"\n', "")
        on = wpo.set_value(base, True, now=1800000000)
        self.assertTrue(on.endswith('[Software\\\\Wine\\\\X11 Driver] 1800000000\n"EmulateModeset"="Y"\n'))
        self.assertIn("[Software\\\\Wine\\\\Direct3D]", on)

    def test_other_value_replaced(self):
        reg = REG.replace('"Decorated"="N"', '"EmulateModeset"="N"\n"Decorated"="N"')
        self.assertIn('"EmulateModeset"="Y"', wpo.set_value(reg, True))
        self.assertNotIn("EmulateModeset", wpo.set_value(reg, False))

    def test_default_files(self):
        with tempfile.TemporaryDirectory() as d:
            for p in ("steamroot/tmp/fexhome/.local/share/Steam/steamapps/compatdata/10/pfx",
                      "arm64root/tmp/armhome/.local/share/Steam/steamapps/compatdata/20/pfx",
                      "steamroot/tmp/fexhome/.steamarm/prefixes/app/pfx"):
                os.makedirs(os.path.join(d, p))
                Path(d, p, "user.reg").write_text(REG)
            files = wpo.default_files(d)
            self.assertEqual(len(files), 3)
            self.assertTrue(all(wpo.apply(f, True) for f in files))
            self.assertFalse(any(wpo.apply(f, True) for f in files))


if __name__ == "__main__":
    unittest.main()
