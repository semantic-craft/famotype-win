# OpenCC standard conversion data

These files are the OpenCC 1.1.9 standard data used by the same Rime dependency
bundle as the Windows `rime.dll` build:

- Upstream: `https://github.com/BYVoid/OpenCC`
- Tag: `ver.1.1.9`
- Tag commit: `556ed22496d650bd0b13b6c163be9814637970ae`
- License: Apache-2.0 (see `LICENSE`)

Only the files needed by Famo's shipped schemas are vendored: `s2t.json` for
rime-ice and `s2hk.json` for Wubi, plus their three referenced `.ocd2`
dictionaries.
