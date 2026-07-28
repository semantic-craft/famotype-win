# Building the Famo engine ABI component

Standalone MSVC/CMake component. Does **not** enter the Weasel pin msbuild and
never locks `weaselx64.dll`.

## Toolchain (this machine)

- CMake 4.3.3
- VS Build Tools 18 (`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`),
  MSVC 14.50 (v145)

## Tier A — one command

```
cd native/windows-tsf-famo/engine-api
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The suite currently runs six checks: legacy roundtrip, ABI-v2 action/recovery,
the deterministic test-engine view contract, Rime smoke, Rime ABI-v2
action/recovery, and the Rime view contract. In particular, the action tests
cover receipt-first `RESYNC_REQUIRED`, bounded `RECOVER`, allocation failure,
candidate/string/result limits, and exact commit recovery without replaying a
business mutation.

These commands only build and load test copies. They do not register a TSF,
replace an installed DLL, restart Weasel, or touch a live input-method process.

If CMake does not auto-pick a generator, pass one explicitly, e.g.
`-G "Visual Studio 17 2022"` or `-G "Visual Studio 18"`.

## Tier B

`FamoRimeEngine.dll` links `C:/fb/weasel/lib64/rime.lib` and includes
`C:/fb/weasel/include`; it needs `rime.dll` on the DLL search path at runtime.
`ctest` runs `rime_smoke` (empty schema, no-crash only).

### Real-candidate verification (manual tool, not a ctest)

`rime_verify` drives `FamoRimeEngine.dll` against a REAL deployed rime data root
and checks that typing yields genuine Chinese candidates:

```
rime_verify.exe <data_root> <schema_id> <ascii_keys>
# e.g. rime_verify.exe C:\some\famo-data rime_ice nihao  ->  [0] 你好 ...
```

Point `<data_root>` at a copy of a deployed data dir (e.g. copy
`%LOCALAPPDATA%\Famo` to a temp dir so the live userdb is not touched — a
partially-copied `*.userdb` just logs a harmless leveldb error and RIME
recovers). Exit 0 iff a multi-byte (CJK) candidate is produced. Not in the
automated suite because it needs deployed schemas absent in CI.
