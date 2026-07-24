# famo-config — 设置面板 ↔ 引擎的配置契约 + payload 组装

> 第一刀切片 · 步骤 1（WSL 可做，不需 Windows 工具链）。
> 架构边界见 ADR-0001/0004。

法墨 Windows = 复用 Weasel TSF 热路径 + WinUI 3 设置面板 + 独立自包含（配置只落
`%LOCALAPPDATA%\Famo`，不碰 `%AppData%\Rime`）。设置面板**写**配置、引擎**读**配置，
本目录就是二者之间的**配置契约**，外加把底座 payload 组装出来的脚本。

## 两档生效模型（PRD §2.3）

| 桶 | 字段组 | 落点 | 生效动作 |
|---|---|---|---|
| **即时(instant)** | 皮肤/明暗/字体/横竖排/内嵌预编辑/圆角边框阴影间距 + 中英标点/全半角/简繁/emoji 开关 | `weasel.custom.yaml` style/* + preset_color_schemes/*、`famo-style.yaml`、`famo-options.yaml` | Weasel 轻量 reload / set_option，**不重部署** |
| **部署(deploy)** | 方案列表/候选数/模糊音/emoji 出厂锚点 | `default.custom.yaml`、`rime_ice.custom.yaml` | 写 yaml → `WeaselDeployer /deploy` 异步重编译 |

## 文件清单

| 文件 | 作用 |
|---|---|
| `famo-settings.schema.json` | Famo 设置 store 的 JSON Schema（draft 2020-12）。描述 `%LOCALAPPDATA%\Famo\famo-settings.json`。每字段标 `x-famo-bucket`(instant/deploy) + `x-famo-target`(目标文件+键)。 |
| `famo-settings.default.json` | 出厂默认实例(已对 schema 校验通过)。skin=shenda、appearanceMode=system、page_size=8、emoji=false、横排、inline_preedit=false、schema 首项 rime_ice(雾凇拼音优先)。 |
| `CONFIG-MAPPING.md` | store 字段 → 目标文件 → 具体键 → 桶 → 生效动作 的总表；四皮肤色值(BGR)。与设计稿 `design/settings/FamoWindowsIME.html` §3 ①②③④ 一一对应。 |
| `overlay/default.custom.yaml` | 法墨方案补丁：`schema_list`(rime_ice 雾凇全套 + wubi86_jidian 极点五笔 4 变体，共 13 方案，雾凇优先) + `menu/page_size: 8`。 |
| `overlay/weasel.custom.yaml` | 候选窗外观 + 四皮肤(默认荔园红 shenda + `color_scheme_dark` 暗色跟随)。 |
| `overlay/rime_ice.custom.yaml` | 方案层出厂锚定：emoji 关（patch `switches`，仅翻 emoji reset）；模糊音出厂全关。tone_display 已下线（rime-ice 无此开关）。 |
| `assemble-payload.sh` | 把 rime-ice 全套 + KyleBing 极点五笔 + overlay 组装到 `payload/`。 |
| `payload/`、`.cache/` | 构建产物 / 克隆缓存，**gitignore**，由脚本生成。 |

## 跑 assemble-payload.sh

```bash
./assemble-payload.sh
# 可选：钉住上游版本
ICE_REF=<tag-or-commit> WUBI_REF=<tag-or-commit> ./assemble-payload.sh
```

脚本做的事(`set -euo pipefail`、幂等、失败即非零退出)：
1. 克隆/更新 `iDvel/rime-ice`（雾凇拼音完整配置：含 cn_dicts/en_dicts/lua/opencc/各双拼/t9/melt_eng）到 `.cache/`。
2. 克隆/更新 `KyleBing/rime-wubi86-jidian`（极点五笔 4 变体 + 反查依赖 pinyin_simp/numbers + 五笔 lua，自洽不依赖拼音底座）到 `.cache/`。
3. 复制 rime-ice 底座到 `payload/`（排除 .git/.github/md/build/recipe.yaml/squirrel.yaml）。
4. 叠加 KyleBing 五笔 overlay（含 _trad 变体 date_translator 短名修正）。
5. 防御式剔除法学层（`famo_law_*`/`wubi86_law*`/`law_*.lua`/`law_phrase*`；底座本不含，纯保险）。
6. 叠加 `overlay/` 三个法墨 custom.yaml（default/weasel/rime_ice）。
7. 体检关键 schema/词典 + 出厂锚定(默认 shenda、page_size 8、含 rime_ice、无 codeLengthLimit/rime_mint 残留)就位。

`payload/` 即首启时 seed 到 `%LOCALAPPDATA%\Famo` 的内置配置包。

网络不可用时脚本会**优雅报错并给出手动克隆步骤**，不会假装成功。

## 许可证（随包须列 THIRD-PARTY-NOTICES，PRD §6.3）

- rime-ice（雾凇）：**GPL-3.0**（词库含腾讯 AI Lab 词向量衍生词库 tencent、通用规范汉字表 8105/41448 等，随 rime-ice 分发）。
- 极点五笔 wubi86_jidian：上游 **KyleBing/rime-wubi86-jidian**（Apache-2.0），脚本单独克隆，自带 pinyin_simp 反查依赖。
- 法墨 overlay + 整库：**GPLv3**（fork Weasel 的连带，PRD §6.3）。

## 不在本切片（需 Windows 工具链）

weasel-fork / settings-winui / installer 见 `../README.md`。本目录**只**定义契约 + 组装 payload，
不碰热路径、不碰其它子目录。
