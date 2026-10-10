# Plugin registry

The registry is a separate repository: `neutro74/umc-registry`. It stores small JSON manifests; `.umc` files are mirrored as GitHub release assets, so a fork does not include every plugin's source.

```text
search coords
versions coords
info coords@1.0.0
install coords@1.0.0
scripts
uninstall coords
```

These commands also work as `umc search`, `umc install`, etc. `search` uses the catalog's display metadata. `info` fetches, hash-checks and compiles one plugin without running it. `install` resolves the full dependency tree, shows permissions and asks for approval before writing files. `--yes` skips that prompt explicitly.

Use `--registry <index.tsv>` on search, info, versions or install, or set `UMC_REGISTRY`. The default is `https://raw.githubusercontent.com/neutro74/umc-registry/main/index.tsv`.

An index has one tab-separated row per version:

```text
id  version  sha256  https-url  percent-encoded-name  percent-encoded-description  owner
```

The first four fields are required; the last three are optional together. Empty display fields use `-`. Whitespace in display fields must be percent-encoded. A local index can reference relative `.umc` paths, which is useful for testing. A remote index must reference HTTPS URLs. Versions are exact; hashes are SHA-256 over the original file bytes.

For sharing, upload a plugin to a public GitHub release or raw repository URL you own. The Pages form reads a selected local file, computes its hash and opens GitHub's new-file editor with a manifest. Submit it as a pull request. Automation accepts additions that pass compiler, ownership, hash, size and dependency checks. It never executes the scripts. Registry code or workflow changes do not enter that acceptance path.

The registry's publishing workflow mirrors accepted packages, generates the index and catalog, and deploys Pages. Its compiler is pinned to a UMC commit. GitHub Pages must use the [GitHub Actions publishing source](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).

Registry deployment is separate from UMC. See the registry README for repository creation, Pages and Actions settings. The default URL becomes available after that repository is created and publishing succeeds.
