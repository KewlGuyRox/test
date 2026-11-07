# Build loop — rules for anyone (human or AI) working this repo

This project has burned hours on avoidable build failures. Read this before
pushing anything.

## Cost

A cold build is **~55 minutes** on `ubuntu-24.04` with `JOBS=4`. There is no
compiler cache in CI today, so every push starts from zero. Treat a build slot
as expensive and batch your changes.

## Rules

1. **Do not push while a build is running.** See "Concurrency" below — a push
   can silently kill a 50-minute build.
2. **Batch changes.** One push with five fixes beats five pushes with one fix.
3. **Never commit CRLF into a script.** A CRLF `build.sh` fails in 1m52s with
   `set: pipefail: invalid option name`. `.gitattributes` sets
   `* text=auto eol=lf`; do not override it.
4. **Do not add a package name to `DEVICE_PACKAGES` that no package defines.**
   `scripts/validate-packages.py` now catches this early, but the check only
   knows what the index knows.
5. **Verify locally what you can before pushing.** Structural checks, grep for
   the ABI/API you just used, and read the header if unsure.

## Concurrency gotcha

The workflow declares:

```yaml
concurrency:
  group: yspeed-openwrt-ramboot-${{ github.ref }}
  cancel-in-progress: true
```

For a **pull request**, GitHub evaluates the *whole PR diff* against the
workflow's `paths` filter — not just the new commit. Since this PR touches
`yspeed-review/**`, **every push to the PR branch restarts the build**, even a
docs-only commit. Two builds were cancelled this way (one at 52 minutes).

Workaround in use: builds are dispatched from an isolated branch instead.

```
branch:   build/verified
trigger:  workflow_dispatch (manual)
```

That gives its own concurrency group, so pushes to the PR branch cannot cancel
it. Only pushes to `build/verified` **and** a new dispatch replace it.

An improved workflow (branch-keyed group, ccache, failure log artifact) is
prepared but cannot be pushed: modifying `.github/workflows/*` requires the
`workflow` OAuth scope, which `gh`'s default token does not request. Use
`gh auth refresh -s workflow` or edit in the web UI.

## Reading a failed build log

Job logs need repo-admin rights, and `gh` needs a writable cache directory. In a
sandboxed session both must be redirected:

```powershell
$env:GH_CONFIG_DIR  = "<workspace>\gh-config"
$env:LOCALAPPDATA   = "<workspace>\gh-cache"
$env:XDG_CACHE_HOME = "<workspace>\gh-cache"
gh run view <run-id> --repo KewlGuyRox/test --log-failed
```

Without the redirect the command fails with
`mkdir ...\AppData\Local\GitHub CLI: Access is denied`.

Then search the log, do not read it end to end — it is ~130k lines:

```powershell
Select-String -Path $log -Pattern '\*\*\* \[.*/compile\] Error'
Select-String -Path $log -Pattern '(error:|undefined reference|No such file)'
```

## Failure history (all distinct, all fixed)

| Symptom | Cause | Fix |
|---|---|---|
| 39.5 min, exit 2 | `DEVICE_PACKAGES` named `kmod-fb-tft-yspeed` / `kmod-fb-tft-st7789v-yspeed`, which no package defines; apk aborted at image assembly | `86c5462` |
| 1m52s, exit 2 | `build.sh` stored with CRLF | `81be2b2`, then repo-wide LF in `0f6181d` |
| 2m30s, exit 1 | `validate-packages.py` regex ran past `DEVICE_PACKAGES` into `SUPPORTED_DEVICES` and comments, reporting them as missing packages | `eed68ec` |
| 54m58s, exit 2 | `ev.time.tv_sec` — `struct input_event` has no `time` member under 64-bit `time_t`; use `input_event_sec` / `input_event_usec` | `2e011ec` |

Note the pattern: three of the four were introduced by a change that was never
compiled. If you cannot compile, at least verify the API you are calling exists
on this target.

## Target-specific traps

- **64-bit `time_t`.** `struct input_event` exposes `input_event_sec` /
  `input_event_usec` macros; `.time` does not compile. Prefer the macros.
- **GPIO sysfs numbering.** Never hardcode; read `gpiochipN/base`. The vendor
  kernel used base 480, mainline 6.12 uses base 0.
- **Shell scripts must be LF**, including extensionless ones such as
  `files/usr/sbin/yspeed-status` and `files/etc/uci-defaults/*`.
- **`IMAGES :=`** suppresses flashable images. The build fails if a
  `*sysupgrade*` or `*factory*` image appears — that is intentional.

## Speed work outstanding

`CONFIG_CCACHE=y` plus a cached `~/.ccache` in the workflow would cut repeat
builds substantially (typically 2–3×). It needs both:

1. `.config`: `CONFIG_CCACHE=y` — a normal repo change, no special rights.
2. `.github/workflows/yspeed-ramboot-build.yml`: cache the ccache directory and
   export `CCACHE_DIR` — this needs the `workflow` scope.

Do both in one change; part 1 alone gains nothing because the cache directory
does not survive between runners.
