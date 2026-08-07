# Artifact Verification Runner Drift Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent unrelated GitHub-hosted runner image updates from rejecting reproducible Perigee artifacts while retaining full verification in the exact producer environment.

**Architecture:** The reusable artifact producer will verify candidate A and candidate B immediately after building and comparing them, while all build packages and source files are still available. The downstream artifact job will compare the four uploaded producer provenance snapshots to each other and compare the downloaded candidate bytes; it will no longer pretend that two independently scheduled runner images have an identical global dpkg database.

**Tech Stack:** GitHub Actions YAML, Python `unittest` packaging-contract tests, Perigee Linux artifact verifier.

## Global Constraints

- Do not weaken artifact content, ELF dependency, license, launch, or reproducibility verification.
- Do not change Sunshine, Polaris, networking, displays, or host services.
- Keep candidate A and candidate B byte-for-byte comparisons.
- Keep producer dpkg provenance attached to every uploaded Linux artifact.

---

### Task 1: Specify producer-local artifact verification

**Files:**
- Modify: `scripts/tests/test_linux_packaging_contract.py`
- Modify: `.github/workflows/build-appimage.yml`

**Interfaces:**
- Consumes: `scripts/verify-linux-artifacts.sh` with `PERIGEE_ARTIFACT_DIR`.
- Produces: verified candidate directories `build/artifacts-a` and `build/artifacts-b` before upload.

- [ ] **Step 1: Write the failing contract test**

Add a test that requires these exact producer commands:

```python
def test_artifact_producer_verifies_both_candidates_before_upload(self) -> None:
    first_upload = self.workflow.index("- name: Upload candidate A AppImage")
    for candidate in ("a", "b"):
        command = (
            f"PERIGEE_ARTIFACT_DIR=\"$GITHUB_WORKSPACE/build/artifacts-{candidate}\" "
            "scripts/verify-linux-artifacts.sh"
        )
        self.assertIn(command, self.workflow)
        self.assertLess(self.workflow.index(command), first_upload)
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest \
  scripts.tests.test_linux_packaging_contract.LinuxPackagingContractTest.test_artifact_producer_verifies_both_candidates_before_upload
```

Expected: FAIL because the producer does not run either verification command.

- [ ] **Step 3: Add producer-local verification**

Add this step after isolated candidate byte comparison and before provenance recording or upload:

```yaml
      - name: Verify isolated release candidates
        if: inputs.job_kind == 'artifacts'
        run: |
          PERIGEE_ARTIFACT_DIR="$GITHUB_WORKSPACE/build/artifacts-a" scripts/verify-linux-artifacts.sh
          PERIGEE_ARTIFACT_DIR="$GITHUB_WORKSPACE/build/artifacts-b" scripts/verify-linux-artifacts.sh
```

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the command from Step 2. Expected: PASS.

### Task 2: Make the downstream provenance gate runner-independent

**Files:**
- Modify: `scripts/tests/test_linux_packaging_contract.py`
- Modify: `.github/workflows/perigee-ci.yml`

**Interfaces:**
- Consumes: four `perigee-dpkg-packages.txt` artifacts uploaded by the producer.
- Produces: a downstream consistency gate that rejects candidate provenance drift without comparing unrelated runner images.

- [ ] **Step 1: Write the failing contract test**

Replace the old verifier-to-producer assertions with:

```python
self.assertIn(
    "reference=ci-provenance/appimage-a/perigee-dpkg-packages.txt",
    verifier_workflow,
)
self.assertIn('cmp --silent "$reference" "$producer"', verifier_workflow)
self.assertIn("producer package provenance differs", verifier_workflow)
self.assertNotIn("verifier-dpkg-packages.txt", verifier_workflow)
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest \
  scripts.tests.test_linux_packaging_contract.LinuxPackagingContractTest.test_ci_uploads_and_matches_producer_dpkg_package_provenance
```

Expected: FAIL because the workflow still compares the verifier runner's complete dpkg database.

- [ ] **Step 3: Compare producer snapshots only**

Replace the old package-set step with:

```yaml
      - name: Match producer package provenance
        run: |
          reference=ci-provenance/appimage-a/perigee-dpkg-packages.txt
          for producer in \
            ci-provenance/tar-a/perigee-dpkg-packages.txt \
            ci-provenance/appimage-b/perigee-dpkg-packages.txt \
            ci-provenance/tar-b/perigee-dpkg-packages.txt; do
            cmp --silent "$reference" "$producer" || {
              printf 'producer package provenance differs: %s\n' "$producer" >&2
              exit 1
            }
          done
```

Remove the downstream verifier package fingerprint and downstream release-verifier steps because both candidates are now verified before upload. Retain downloaded byte comparison and executable-mode restoration.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the command from Step 2. Expected: PASS.

### Task 3: Verify, publish, and deploy the next safe client artifact

**Files:**
- Verify: `.github/workflows/build-appimage.yml`
- Verify: `.github/workflows/perigee-ci.yml`
- Verify: `scripts/tests/test_linux_packaging_contract.py`

**Interfaces:**
- Consumes: the complete packaging and verifier unit suite plus GitHub Actions artifacts.
- Produces: a verified AppImage that can replace only the Perigee client on Cometforge with a rollback copy.

- [ ] **Step 1: Run the full packaging contract**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest discover \
  -s scripts/tests -p 'test_*.py'
```

Expected: all tests PASS.

- [ ] **Step 2: Commit and push the scoped CI correction**

```bash
git add .github/workflows/build-appimage.yml .github/workflows/perigee-ci.yml \
  scripts/tests/test_linux_packaging_contract.py \
  docs/superpowers/plans/2026-08-07-artifact-verification-runner-drift.md
git commit -m "Verify Linux artifacts in their producer environment"
git push origin main
```

- [ ] **Step 3: Require both GitHub Actions workflows to pass**

Check the Build and Perigee CI runs for the new commit. Expected: both conclusions are `success`, producer-local candidate verification passes, producer provenance matches, and candidate bytes match.

- [ ] **Step 4: Verify and deploy only the Perigee client**

Download candidate A to a private temporary directory, verify SHA-256 and `Perigee 0.1.0`, confirm both Qt XCB OpenGL integration plugins are present, and confirm `libxcb-glx.so.0` remains host-provided. If no Perigee process is running on Cometforge, preserve the current wrapper target as rollback, install the verified AppImage, validate the desktop entry, and launch it through the desktop entry. Do not change any server or host service.
