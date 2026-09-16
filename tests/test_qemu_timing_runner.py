"""Host-side fail-closed checks for the QEMU timing gate itself."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / "scripts/run_qemu_timing.py"
spec = importlib.util.spec_from_file_location("timing_runner", path)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

class RunnerTest(unittest.TestCase):
    def test_exact_final_pass(self):
        self.assertIsNone(runner.validate_result(0, "stats: good\r\nBUDGET: PASS\r\n", "BUDGET: PASS"))

    def test_failures_are_not_retried_or_accepted(self):
        cases = [(124, "BUDGET: PASS"), (-9, "BUDGET: PASS"), (1, "BUDGET: PASS"),
                 (0, ""), (0, "almost BUDGET: PASS"),
                 (0, "BUDGET: FAIL\nBUDGET: PASS"), (0, "PANIC\nBUDGET: PASS"),
                 (0, "BUDGET: PASS\nBUDGET: PASS"), (0, "BUDGET: PASS\nlate error")]
        for code, text in cases:
            with self.subTest(code=code, text=text):
                self.assertIsNotNone(runner.validate_result(code, text, "BUDGET: PASS"))

    def test_explicit_clock_models(self):
        common = ("qemu", Path("kernel.elf"), Path("serial.log"), "budget")
        det = runner.command(*common, "deterministic", 3)
        wall = runner.command(*common, "wallclock", 3)
        self.assertIn("shift=3,align=off,sleep=off", det)
        self.assertIn("tcg,thread=single", det)
        self.assertIn("tcg,thread=multi", wall)
        self.assertNotIn("-icount", wall)
        with self.assertRaises(ValueError):
            runner.command(*common, "auto", 3)

    def test_positive_repetition_count(self):
        self.assertEqual(runner.positive("20"), 20)
        for value in ("0", "-1"):
            with self.assertRaises(Exception):
                runner.positive(value)

if __name__ == "__main__":
    unittest.main()
