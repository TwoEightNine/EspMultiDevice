# Contributing to ESP Media

Thanks for your interest in ESP Media. Bug reports, fixes, documentation
improvements, ports to other hardware, and new SD apps are all welcome.
Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md).

ESP Media is a hobby project maintained in spare time, so reviews may take a
while. Small, focused changes are the quickest to review.

## Reporting bugs and requesting features

- Search the existing issues first.
- In a bug report, include the firmware version from Settings → About, what
  happened and the steps to reproduce it, how your hardware differs from the
  [default wiring](docs/hardware.md#default-wiring), and the 115200-baud Serial
  log around the problem.
- For a larger feature or behavior change, open an issue to discuss it before
  writing code.
- Report security problems privately as described in [SECURITY.md](SECURITY.md),
  not in a public issue.

## Setting up

Follow [Setup and build](README.md#setup-and-build) in the README. Then build
with:

```bash
tools/build_esp32s3.sh
```

Add `--profile` to build against the exact board package and library
versions pinned in `sketch.yaml`. On Windows, run the scripts in Git Bash.
[docs/development.md](docs/development.md) covers build options, bundled
dependencies, generators, and test sketches.

## Making a change

1. Read [AGENTS.md](AGENTS.md). Its UI contract and implementation rules are
   mandatory for every contributor, human or AI, and it tells you which
   document defines the contract for the area you are changing.
2. Match the style, naming, and comment density of the surrounding code. Keep
   UI-loop work non-blocking, and use `SpiBusLock` for SD access.
3. Update the focused `docs/*.md` file whenever behavior, a persistent format,
   wiring, or a public contract changes. Keep README summaries short.
4. Add a line under **Unreleased** in [CHANGELOG.md](CHANGELOG.md) for any
   user-visible change. Don't bump the firmware version; the maintainer does
   that when releasing.

### Vendored code

- Don't edit code under `third_party/` unless the change requires it. Prefer
  adapters in `src/vendor_shims/`.
- A new or upgraded dependency must have a GPL-3.0-compatible license. Keep its
  notices, record its version and source, and update
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- After importing or upgrading FFmpeg or OpenCORE AMR, run
  `python3 tools/localize_vendor_includes.py`. Never add headers or symlinks
  to the sketch root.

### Testing

- Every C++ or configuration change must build with `tools/build_esp32s3.sh`,
  which also checks the application-slot size limit.
- Assistant protocol changes must also build with
  `tools/build_assistant_contract.sh`.
- Media changes should exercise a representative file and any cache
  invalidation path. Regenerate fixtures only with their documented tools.
- If you can, test on a device. Either way, say in the pull request what you
  tested and on which hardware.

## Pull requests

- Keep each pull request to one topic, and explain what changed and why.
- Say how you tested the change: which builds you ran, and what you checked on
  which hardware.
- AI-assisted contributions are fine. You are responsible for having reviewed
  and tested everything you submit.

## License of contributions

ESP Media is licensed under the GNU General Public License version 3 or later,
with the additional permission for the Helix AAC decoder given in the
[README](README.md#license). By submitting a contribution, you agree that it is
licensed under those same terms and that you have the right to submit it.

The TwoEightNine logo is not GPL-licensed and may not be modified; see
[src/ui/LICENSE.logo](src/ui/LICENSE.logo).
