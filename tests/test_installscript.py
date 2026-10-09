"""tools/steamarm-native-proton/installscript.py: an evaluator script's run-process
steps become "has run" registry values in both views; nothing to mark exits 2."""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "steamarm-native-proton", "installscript.py")

SCRIPT = r'''"evaluatorscript"
{
	"0"
	{
		"appid"		"100"
		"compat_installscript"
		{
			"run process"
			{
				"GamingRuntime"
				{
					"hasrunkey"		"HKEY_LOCAL_MACHINE\\Software\\Some Game\\GamingRepair"
					"process 1"		"cmd.exe"
				}
				"NoKey"
				{
					"process 1"		"setup.exe"
				}
			}
			"Firewall"
			{
				"Some Game"		"%INSTALLDIR%\\game.exe"
			}
		}
	}
	"1"
	{
		"appid"		"228980"
		"compat_installscript"
		{
			"Run Process"
			{
				"x64 14.51"
				{
					"HasRunKey"		"HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Valve\\Steam\\Apps\\CommonRedist\\vcredist\\2022"
					"RunKeyName"		"x64 redist"
					"MinimumHasRunValue"		"3"
				}
			}
		}
	}
}
'''


class InstallScript(unittest.TestCase):
    def run_tool(self, text):
        d = tempfile.mkdtemp()
        vdf, reg = os.path.join(d, "evaluatorscript_100.vdf"), os.path.join(d, "out.reg")
        with open(vdf, "w") as f:
            f.write(text)
        p = subprocess.run([sys.executable, TOOL, vdf, reg], capture_output=True, text=True)
        out = open(reg).read() if os.path.exists(reg) else None
        return p.returncode, out, p.stderr

    def test_steps_marked_in_both_views(self):
        rc, reg, err = self.run_tool(SCRIPT)
        self.assertEqual(rc, 0, err)
        self.assertTrue(reg.startswith("REGEDIT4"))
        self.assertIn("[HKEY_LOCAL_MACHINE\\Software\\Some Game\\GamingRepair]\n\"GamingRuntime\"=dword:00000001", reg)
        self.assertIn("[HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Some Game\\GamingRepair]\n\"GamingRuntime\"=dword:00000001", reg)
        # an explicit RunKeyName and minimum; a key already in the 32-bit view is not doubled
        self.assertIn("[HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Valve\\Steam\\Apps\\CommonRedist\\vcredist\\2022]\n\"x64 redist\"=dword:00000003", reg)
        self.assertNotIn("Wow6432Node\\Wow6432Node", reg)
        # steps without a has-run key and other sections are left alone
        self.assertNotIn("NoKey", reg)
        self.assertNotIn("Firewall", reg)
        self.assertEqual(err.count("marked done"), 2)

    def test_nothing_to_mark(self):
        rc, reg, _ = self.run_tool('"evaluatorscript" { "0" { "appid" "1" "compat_installscript" { } } }')
        self.assertEqual(rc, 2)
        self.assertIsNone(reg)


if __name__ == "__main__":
    unittest.main()
