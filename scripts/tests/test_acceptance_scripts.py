#!/usr/bin/env python3

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import textwrap
import unittest


SOURCE_ROOT = pathlib.Path(__file__).resolve().parents[2]
COLLECT_ENVIRONMENT = SOURCE_ROOT / "scripts/acceptance/collect-environment.sh"
DECK_CYCLE_TEST = SOURCE_ROOT / "scripts/acceptance/deck-cycle-test.sh"


class AcceptanceScriptsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.environment = {
            "HOME": str(self.root / "home"),
            "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8",
            "PATH": f"{self.bin}:/usr/bin:/bin",
            "PYTHONDONTWRITEBYTECODE": "1",
            "XDG_CURRENT_DESKTOP": "KDE",
            "XDG_SESSION_TYPE": "wayland",
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_executable(self, name: str, body: str) -> pathlib.Path:
        path = self.bin / name
        path.write_text("#!/bin/sh\nset -eu\n" + textwrap.dedent(body), encoding="utf-8")
        path.chmod(0o755)
        return path

    def run_script(
        self,
        script: pathlib.Path,
        *arguments: str,
        extra_environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        environment = dict(self.environment)
        if extra_environment:
            environment.update(extra_environment)
        return subprocess.run(
            ["bash", str(script), *arguments],
            cwd=SOURCE_ROOT,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def install_environment_commands(self) -> pathlib.Path:
        self.write_executable("qmake6", 'printf "%s\\n" "6.8.3"\n')
        self.write_executable("sdl2-config", 'printf "%s\\n" "2.30.7"\n')
        self.write_executable(
            "glxinfo",
            """
            printf '%s\n' \
              'OpenGL renderer string: Fixture GPU at 192.0.2.44' \
              'Route endpoint 2001:db8::44' \
              'MAC: 02:00:5e:10:00:00' \
              'Adapter 02-00-5e-10-00-01' \
              'Object 01890f5e-7b8a-7cc0-98f1-426614174002' \
              'Source address: 198.51.100.44' \
              'certificate fingerprint: CERTIFICATE_FINGERPRINT_CANARY' \
              'token=PERIGEE_TOKEN_CANARY' \
              '-----BEGIN CERTIFICATE-----' \
              'PERIGEE_CERTIFICATE_CANARY' \
              '-----END CERTIFICATE-----'
            """,
        )
        self.write_executable(
            "kscreen-doctor",
            """
            printf '%s\n' \
              'Output: DP-1 UUID 123e4567-e89b-12d3-a456-426614174000' \
              'Mode: 2560x1440@144'
            """,
        )
        self.write_executable("polaris", 'printf "%s\\n" "Polaris 1.3.1"\n')
        self.write_executable("git", 'printf "%s\\n" "0123456789abcdef0123456789abcdef01234567"\n')
        marker = self.root / "network-command-ran"
        for command in ("curl", "nc", "ping", "ssh", "wget"):
            self.write_executable(
                command,
                f'printf "%s\\n" "{command}" >> "$PERIGEE_TEST_NETWORK_MARKER"\nexit 97\n',
            )
        return marker

    def test_environment_collection_redacts_sensitive_values_without_network_commands(self) -> None:
        marker = self.install_environment_commands()
        result = self.run_script(
            COLLECT_ENVIRONMENT,
            extra_environment={
                "PERIGEE_TEST_NETWORK_MARKER": str(marker),
                "PERIGEE_COLLECT_GLXINFO": "YES",
                "PERIGEE_COLLECT_DISPLAY_OUTPUTS": "YES",
                "DISPLAY": ":fixture",
            },
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Qt version: 6.8.3", result.stdout)
        self.assertIn("SDL version: 2.30.7", result.stdout)
        self.assertIn("<redacted-address>", result.stdout)
        self.assertIn("<redacted-uuid>", result.stdout)
        self.assertIn("<redacted-certificate>", result.stdout)
        self.assertIn("<redacted-token>", result.stdout)
        for canary in (
            "192.0.2.44",
            "2001:db8::44",
            "02:00:5e:10:00:00",
            "02-00-5e-10-00-01",
            "123e4567-e89b-12d3-a456-426614174000",
            "01890f5e-7b8a-7cc0-98f1-426614174002",
            "198.51.100.44",
            "CERTIFICATE_FINGERPRINT_CANARY",
            "PERIGEE_TOKEN_CANARY",
            "PERIGEE_CERTIFICATE_CANARY",
        ):
            self.assertNotIn(canary, result.stdout + result.stderr)
        self.assertFalse(marker.exists(), "collector invoked a network command")

    def test_paired_summary_requires_flag_and_environment_gate_then_redacts(self) -> None:
        self.install_environment_commands()
        summary = self.root / "paired-summary.txt"
        summary.write_text(
            "permissions=display.switch,clipboard.write\n"
            "host_address=198.51.100.25\n"
            "session_token=PAIRED_SUMMARY_TOKEN_CANARY\n"
            "client_certificate=PAIRED_SUMMARY_CERTIFICATE_CANARY\n"
            "server_uuid=123e4567-e89b-12d3-a456-426614174001\n"
            "private key: PAIRED_PRIVATE_KEY_CANARY\n"
            "client certificate fingerprint: PAIRED_CERT_FINGERPRINT_CANARY\n",
            encoding="utf-8",
        )

        denied = self.run_script(
            COLLECT_ENVIRONMENT,
            "--include-paired-summary",
            str(summary),
        )
        self.assertNotEqual(denied.returncode, 0)
        self.assertNotIn("display.switch", denied.stdout + denied.stderr)
        self.assertNotIn("PAIRED_SUMMARY_TOKEN_CANARY", denied.stdout + denied.stderr)

        allowed = self.run_script(
            COLLECT_ENVIRONMENT,
            "--include-paired-summary",
            str(summary),
            extra_environment={"PERIGEE_ACCEPT_SENSITIVE_COLLECTION": "YES"},
        )
        self.assertEqual(allowed.returncode, 0, allowed.stderr)
        self.assertIn("permissions=display.switch,clipboard.write", allowed.stdout)
        self.assertIn("<redacted-address>", allowed.stdout)
        self.assertIn("<redacted-token>", allowed.stdout)
        self.assertNotIn("198.51.100.25", allowed.stdout + allowed.stderr)
        self.assertNotIn("PAIRED_SUMMARY_TOKEN_CANARY", allowed.stdout + allowed.stderr)
        self.assertNotIn("PAIRED_SUMMARY_CERTIFICATE_CANARY", allowed.stdout + allowed.stderr)
        self.assertNotIn("123e4567-e89b-12d3-a456-426614174001", allowed.stdout + allowed.stderr)
        self.assertNotIn("PAIRED_PRIVATE_KEY_CANARY", allowed.stdout + allowed.stderr)
        self.assertNotIn("PAIRED_CERT_FINGERPRINT_CANARY", allowed.stdout + allowed.stderr)

    def test_deck_cycle_defaults_to_dry_run_without_invoking_driver(self) -> None:
        log = self.root / "driver.log"
        driver = self.write_executable(
            "deck-driver",
            'printf "%s\\n" "$1" >> "$PERIGEE_TEST_DRIVER_LOG"\n',
        )
        result = self.run_script(
            DECK_CYCLE_TEST,
            extra_environment={
                "PERIGEE_DECK_CYCLE_DRIVER": str(driver),
                "PERIGEE_TEST_DRIVER_LOG": str(log),
            },
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DRY RUN", result.stdout)
        self.assertIn("automated cycles: 100", result.stdout)
        self.assertFalse(log.exists(), "dry run invoked the live driver")

    def test_deck_cycle_live_flag_requires_environment_gate(self) -> None:
        log = self.root / "driver.log"
        driver = self.write_executable(
            "deck-driver",
            'printf "%s\\n" "$1" >> "$PERIGEE_TEST_DRIVER_LOG"\n',
        )
        result = self.run_script(
            DECK_CYCLE_TEST,
            "--live",
            "--cycles",
            "1",
            extra_environment={
                "PERIGEE_DECK_CYCLE_DRIVER": str(driver),
                "PERIGEE_TEST_DRIVER_LOG": str(log),
            },
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("PERIGEE_ACCEPT_LIVE_TESTS=YES", result.stderr)
        self.assertFalse(log.exists(), "denied live run invoked the driver")

    def test_deck_cycle_live_run_compares_neutral_snapshots(self) -> None:
        log = self.root / "driver.log"
        driver = self.write_executable(
            "deck-driver",
            """
            printf '%s\n' "$1" >> "$PERIGEE_TEST_DRIVER_LOG"
            case "$1" in
              snapshot) printf '%s\n' 'keyboard=up mouse=up gamepad=up' ;;
              open|close) ;;
              *) exit 64 ;;
            esac
            """,
        )
        result = self.run_script(
            DECK_CYCLE_TEST,
            "--live",
            "--cycles",
            "2",
            extra_environment={
                "PERIGEE_ACCEPT_LIVE_TESTS": "YES",
                "PERIGEE_DECK_CYCLE_DRIVER": str(driver),
                "PERIGEE_TEST_DRIVER_LOG": str(log),
            },
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS: 2 automated cycles", result.stdout)
        self.assertEqual(
            log.read_text(encoding="utf-8").splitlines(),
            ["snapshot", "open", "close", "open", "close", "snapshot"],
        )

    def test_deck_cycle_closes_after_a_mutating_open_failure(self) -> None:
        log = self.root / "driver.log"
        driver = self.write_executable(
            "deck-driver",
            """
            printf '%s\n' "$1" >> "$PERIGEE_TEST_DRIVER_LOG"
            case "$1" in
              snapshot) printf '%s\n' 'keyboard=up mouse=up gamepad=up' ;;
              open) exit 17 ;;
              close) ;;
              *) exit 64 ;;
            esac
            """,
        )
        result = self.run_script(
            DECK_CYCLE_TEST,
            "--live",
            "--cycles",
            "1",
            extra_environment={
                "PERIGEE_ACCEPT_LIVE_TESTS": "YES",
                "PERIGEE_DECK_CYCLE_DRIVER": str(driver),
                "PERIGEE_TEST_DRIVER_LOG": str(log),
            },
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(
            log.read_text(encoding="utf-8").splitlines(),
            ["snapshot", "open", "close"],
        )

    def test_deck_cycle_rejects_changed_input_state_without_printing_snapshot(self) -> None:
        log = self.root / "driver.log"
        state = self.root / "driver.state"
        driver = self.write_executable(
            "deck-driver",
            """
            printf '%s\n' "$1" >> "$PERIGEE_TEST_DRIVER_LOG"
            case "$1" in
              snapshot)
                if [ -e "$PERIGEE_TEST_DRIVER_STATE" ]; then
                  printf '%s\n' 'SECRET_STUCK_INPUT_CANARY'
                  printf '%s\n' 'SECRET_DRIVER_STDERR_CANARY' >&2
                else
                  : > "$PERIGEE_TEST_DRIVER_STATE"
                  printf '%s\n' 'keyboard=up mouse=up gamepad=up'
                fi
                ;;
              open|close) ;;
              *) exit 64 ;;
            esac
            """,
        )
        result = self.run_script(
            DECK_CYCLE_TEST,
            "--live",
            "--cycles",
            "1",
            extra_environment={
                "PERIGEE_ACCEPT_LIVE_TESTS": "YES",
                "PERIGEE_DECK_CYCLE_DRIVER": str(driver),
                "PERIGEE_TEST_DRIVER_LOG": str(log),
                "PERIGEE_TEST_DRIVER_STATE": str(state),
            },
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("input state changed", result.stderr)
        self.assertNotIn("SECRET_STUCK_INPUT_CANARY", result.stdout + result.stderr)
        self.assertNotIn("SECRET_DRIVER_STDERR_CANARY", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
