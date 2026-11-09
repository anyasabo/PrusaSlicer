# Fork anyasabo/PrusaSlicer: branches and releases

## Branches

| Branch | What | Rules |
| --- | --- | --- |
| `master` | Follows `prusa3d/PrusaSlicer` master | Never push to it |
| `sequential-print` (and other topic branches) | One upstream pull request each, on `upstream/master` | No fork files (no `.github/fork`, no `version.inc` change). Do not force-push |
| `anya/main` | The build that people use: `upstream/master` + a merge of each topic branch + the fork files | Merge, do not rebase. Only the fork files are commits of their own |

To update `anya/main`: merge the new upstream base (or a new upstream release tag), then merge each topic branch
again. A topic branch that upstream accepted is not merged again.

## Build

The same scripts run locally and in CI (`.github/workflows/fork-build.yml`):

```sh
.github/fork/build.sh all          # doc/Build.md step 1 and 2, install into build/dist
.github/fork/package-linux.sh      # or package-macos.sh: packages into build/package
```

With prebuilt dependencies: `DEPS_PREFIX="/path/a;/path/b" .github/fork/build.sh app`.

## Release

1. Tag `anya/main`: `git tag -a v3.0.0-alpha14-anya.1 -m "..."`, then push the tag.
2. CI builds macOS arm64 and Linux x86_64 and uploads them to the release of the tag (a pre-release).
3. A local build can also upload: `.github/fork/upload-release.sh v3.0.0-alpha14-anya.1` (it uses `gh`).

The version is upstream `SLIC3R_VERSION`, with the suffix after `+` in the build ID (see `version.inc`). The suffix
comes from the tag: `anya.1`. A build without a tag gets `anya-<commit>`.
