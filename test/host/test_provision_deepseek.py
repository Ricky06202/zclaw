import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import provision_deepseek


class ProvisionDeepSeekTests(unittest.TestCase):
    def test_loads_key_from_deepseek_provider_without_output(self):
        api_key = "sk-test-deepseek-credential-123"
        with tempfile.TemporaryDirectory() as temp_dir:
            auth_file = Path(temp_dir) / "auth.json"
            auth_file.write_text(
                json.dumps({"providers": {"deepseek": {"auth": {"key": api_key}}}}),
                encoding="utf-8",
            )

            self.assertEqual(provision_deepseek.load_deepseek_api_key(auth_file), api_key)

    def test_missing_provider_key_is_reported_safely(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            auth_file = Path(temp_dir) / "auth.json"
            auth_file.write_text('{"providers": {}}', encoding="utf-8")

            with self.assertRaisesRegex(
                provision_deepseek.ProvisionError, "not found in the local provider store"
            ):
                provision_deepseek.load_deepseek_api_key(auth_file)

    def test_builds_single_line_provision_frame(self):
        api_key = "sk-test-deepseek-credential-123"
        self.assertEqual(
            provision_deepseek.build_frame(api_key),
            provision_deepseek.PROVISION_PREFIX + api_key.encode("ascii") + b"\n",
        )

    def test_rejects_control_characters_and_oversized_keys(self):
        with self.assertRaises(provision_deepseek.ProvisionError):
            provision_deepseek.build_frame("sk-test-key-with-newline\n")
        with self.assertRaises(provision_deepseek.ProvisionError):
            provision_deepseek.build_frame("x" * (provision_deepseek.MAX_KEY_LENGTH + 1))

    def test_firmware_probe_prevents_sending_key_to_unsupported_firmware(self):
        fake_serial = _FakeSerial(
            [b"console output " + provision_deepseek.RESPONSE_PREFIX + b"OK\r\n"]
        )
        serial_module = SimpleNamespace(Serial=lambda *args, **kwargs: fake_serial)
        api_key = "sk-test-deepseek-credential-123"

        with patch.dict(sys.modules, {"serial": serial_module}), patch(
            "provision_deepseek.time.sleep"
        ):
            with self.assertRaisesRegex(
                provision_deepseek.ProvisionError, "no credential sent"
            ):
                provision_deepseek.provision_device(api_key, "/dev/fake")

        self.assertEqual(fake_serial.writes, [provision_deepseek.PROVISION_PREFIX + b"probe\n"])

    def test_provisions_only_after_keyless_probe_is_rejected(self):
        fake_serial = _FakeSerial(
            [
                b"console output " + provision_deepseek.RESPONSE_PREFIX + b"ERR\r\n",
                b"console output " + provision_deepseek.RESPONSE_PREFIX + b"OK\r\n",
            ]
        )
        serial_module = SimpleNamespace(Serial=lambda *args, **kwargs: fake_serial)
        api_key = "sk-test-deepseek-credential-123"

        with patch.dict(sys.modules, {"serial": serial_module}), patch(
            "provision_deepseek.time.sleep"
        ):
            provision_deepseek.provision_device(api_key, "/dev/fake")

        self.assertFalse(fake_serial.dtr)
        self.assertFalse(fake_serial.rts)
        self.assertEqual(
            fake_serial.writes,
            [
                provision_deepseek.PROVISION_PREFIX + b"probe\n",
                provision_deepseek.build_frame(api_key),
            ],
        )


class _FakeSerial:
    def __init__(self, responses):
        self.responses = list(responses)
        self.writes = []
        self.dtr = True
        self.rts = True

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def flush(self):
        pass

    def readline(self):
        if self.responses:
            return self.responses.pop(0)
        return b""


if __name__ == "__main__":
    unittest.main()
