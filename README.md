# Child bootstrap

`app.stokerhq.child-bootstrap`: a [Stoker](https://github.com/StokerHQ) hook plugin that carries a Stoker bootstrap into a
child process that would never load one itself - a program started by another program, as a launcher in front of the
program it starts does - and hands the running session over to it, all before the child runs any code of its own.

## What it does

- Detours `CreateProcessW` in the process it runs in (only in `process`, when that is set).
- A child whose executable is `executable` is created suspended, whatever the caller asked for. The bootstrap named
  `bootstrap` in the child's own folder is loaded into it, and the bootstrap's startup pass runs there: plugins
  initialised, detours enabled. Only then does the child's first thread run.
- Before releasing the child, it asks the Stoker launcher over its local pipe (`session.transfer`) to carry the run over
  to the child: one run, one play-time clock, saves collected when the child exits. The launcher checks the request
  against the kernel's own record of the processes; a refusal or a timeout is logged and the child runs anyway.
- The caller's creation flags, environment, handles and working directory are kept. When anything fails, the child is
  resumed untouched and runs as it would have without Stoker.

Windows only (x86 and x64); the child must have the caller's architecture.

## Options

| Option | | |
|---|---|---|
| `executable` | required | File name of the child to take. |
| `bootstrap` | required | File name of the bootstrap to load, from the child's own folder. Its `stoker_hooks.json` is the child's configuration. |
| `process` | optional | Only act inside this executable. |

`src/remote_bootstrap.h` (the remote load and startup pass) and `src/agent_pipe_windows.h` (the launcher pipe's name
and owner check) are the same code the Stoker launcher uses for its side of both.
## Releases

A release is tagged `v<version>`, the version the manifest in the source declares; CI refuses a tag that differs. The
release's assets are built by CI from the tagged commit (`child-bootstrap-windows-x86.dll`, `child-bootstrap-windows-x64.dll`) and are what the StokerHQ plugin registry
(`app.stokerhq.child-bootstrap` in [StokerHQ/plugins](https://github.com/StokerHQ/plugins)) lists by SHA-256 and BLAKE3, signed by the
StokerHQ publisher key.

## Building

```powershell
./build.ps1
```

Needs Visual Studio with the C++ workload. The SDK is the release `stoker-plugin.yaml` pins, fetched once and checked by
SHA-256; `STOKER_PLUGIN_SDK` names a local SDK folder or zip instead.

## Licence

AGPL-3.0-or-later (`LICENSE`).