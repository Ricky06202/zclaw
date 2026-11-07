"""Guard the serial provisioner against echoing credential material."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
PROVISION_SCRIPT = PROJECT_ROOT / "scripts" / "provision.sh"
SECRET_NAMES = ("WIFI_PASS", "API_KEY", "TG_TOKEN")


class ProvisionSecretTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.lines = PROVISION_SCRIPT.read_text(encoding="utf-8").splitlines()

    def test_interactive_secret_prompts_disable_terminal_echo(self) -> None:
        secret_reads = [
            line.strip()
            for line in self.lines
            if re.search(r"\bread\b", line)
            and any(re.search(rf"\b{name}$", line.strip()) for name in SECRET_NAMES)
        ]
        self.assertTrue(secret_reads, "expected at least one interactive secret prompt")
        for prompt in secret_reads:
            with self.subTest(prompt=prompt):
                self.assertIn("-s", prompt)

    def test_terminal_output_does_not_interpolate_secret_values(self) -> None:
        output_lines = [
            line.strip()
            for line in self.lines
            if line.lstrip().startswith(("echo ", "printf "))
            and "csv_escape" not in line
        ]
        for line in output_lines:
            for name in SECRET_NAMES:
                with self.subTest(line=line, secret=name):
                    self.assertIsNone(re.search(rf"\${{{name}(?:[:}}])|\${name}\b", line))

    def test_secret_environment_inputs_are_supported(self) -> None:
        source = "\n".join(self.lines)
        self.assertIn('WIFI_PASS="${ZCLAW_WIFI_PASS:-}"', source)
        self.assertIn('API_KEY="${ZCLAW_API_KEY:-}"', source)


if __name__ == "__main__":
    unittest.main()
