# Creating a release

Releases are entirely tag-driven — there is no manual "draft a release" step in GitHub. Pushing a
git tag matching `v*` triggers `.github/workflows/build.yml`, which:

1. Builds every non-deprecated `UART`/`STLINK` target across `targets/*.ini`.
2. Publishes a GitHub Release named after the tag, with `generate_release_notes: true` and all
   built `firmware.bin`/`firmware.bin.gz` files attached (plus ESP32 partition/bootloader files
   where applicable).

The release job only runs when the pushed ref is `refs/tags/v*` — regular branch pushes just build
and stop. The tag does **not** need to be on `master`; tagging any branch tip and pushing that tag
works, since the workflow triggers on tag pushes regardless of which branch they point at.

## Steps

1. Pick a version. Existing tags follow plain semver with a `v` prefix (`v5.0.0`, `v4.1.0`), with
   `-rcN` or `-beta` suffixes for pre-release builds (e.g. `v5.0.0-rc1`, `v5.0.0-beta`).

2. Decide which remote to push to — this determines where the release is published:
   - `fork` (`adhishvyas/FormationFlight`) — publishes on your fork, does not affect upstream.
   - `origin` (`FormationFlight/FormationFlight`) — publishes on the upstream project, visible to
     all users of the firmware.

3. Tag the current commit and push the tag:
   ```
   git tag <version>
   git push <remote> <version>
   ```
   For example, from `follow_on_inav`, to publish a beta on your fork:
   ```
   git tag v5.0.0-beta
   git push fork v5.0.0-beta
   ```

4. Watch the Actions run on the target repo (Actions tab on GitHub). Once the `build` job finishes
   for all targets, the `release` job downloads the artifacts and publishes the GitHub Release.

## Notes

- If you need to redo a release under the same version, delete the tag both locally and on the
  remote first (`git tag -d <version>` and `git push <remote> :refs/tags/<version>`), otherwise the
  push is rejected. Deleting a tag on GitHub does not delete the release itself — that must be
  removed separately from the Releases page if you want a clean slate.
- The `build` job also runs (without releasing) on every push to `master` and on every pull request
  into `master`, so ordinary CI is unaffected by this process.
