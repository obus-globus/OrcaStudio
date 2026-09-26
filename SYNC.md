# Keeping this fork in sync

This is a GitHub fork of jarczakpawel/OrcaStudio whose `main` also carries real OrcaSlicer
history, so both upstreams can be merged with normal git tooling.

OrcaStudio's own history is squashed and unrelated to OrcaSlicer's. To fix that, OrcaStudio's
changes were re-imported onto OrcaSlicer history (`ostudio-import`), OrcaSlicer main was merged
in, and the result was joined to OrcaStudio's `main` with a merge commit whose tree is the
OrcaSlicer-based one. `main` therefore has both histories as ancestors.

## Branch layout

- `main` — OrcaStudio + current OrcaSlicer main. Build and release from here.
- `ostudio-import` — OrcaSlicer `d6cb667` + OrcaStudio's own changes only
  (curated import of OrcaStudio `831f5a4a`, then its later commits cherry-picked).

The import dropped OrcaStudio files that were only whitespace/mode noise or that are
byte-/whitespace-identical to an older OrcaSlicer revision (stale leftovers, including
all `resources/profiles` diffs). See the import commit message.

## Remotes

```sh
git remote add upstream https://github.com/OrcaSlicer/OrcaSlicer.git
git remote add ostudio  https://github.com/jarczakpawel/OrcaStudio.git   # the fork parent
git config rerere.enabled true
git config merge.conflictStyle zdiff3
```

## Pulling new OrcaSlicer changes

```sh
git fetch upstream
git checkout main
git merge upstream/main
```

Hot spots: `GUI_App.cpp`, `BBLNetworkPlugin.*`, `BBLPrinterAgent.*`, `bambu_networking.hpp`,
`MediaPlayCtrl.*`, `DevManager.*`, `CMakeLists.txt` (FFmpeg staging), `deps/`.
Move any new upstream workflow in `.github/workflows/` to `.github/workflows_disabled/`
unless it is wanted here (scheduled/push workflows burn Actions minutes).

## Pulling new OrcaStudio changes

`main` already has OrcaStudio's `main` as an ancestor, so ordinary commits merge directly:

```sh
git fetch ostudio
git checkout main
git merge ostudio/main
```

Their commits touch OrcaStudio-owned code, so conflicts should be rare. Stale OrcaSlicer files
they still carry are not a problem: git sees them as unchanged on their side.

If OrcaStudio rebases onto a newer OrcaSlicer again (a huge "update" commit), don't merge
that commit. Diff its tree against the OrcaSlicer commit it claims and take only real changes,
the same way the original import did.

## Invariants to keep

- The Linux runtime forwarder/host (`src/slic3r/Utils/SlicerLinuxRuntime/`,
  `tools/slicer_linux_runtime_host/`) implements only the current plug-in ABI
  (`NetworkAbi::Current`). `BBLNetworkPlugin::initialize` refuses other series when the
  runtime is enabled; keep that check if upstream adds more ABI generations.
- `version.inc` stays at the Bambu Studio version (`02.08.01.55`); the plug-in is versioned against it.
- The camera uses OrcaStudio's `wxMediaCtrl3`/`AVVideoDecoder` (newer Bambu Studio 02.08 port), not upstream's.

## Building (Linux)

```sh
./build_linux.sh -u        # system packages
./build_linux.sh -d -j 4   # deps
./build_linux.sh -s -j 4   # app
./build_linux.sh -i        # AppImage -> build/OrcaStudio_Linux_*.AppImage
```
