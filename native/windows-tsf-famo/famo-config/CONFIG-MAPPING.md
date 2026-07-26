# CONFIG-MAPPING · Famo 设置 store ↔ RIME/Weasel 配置键

> 设置面板(WinUI 3)写 `%LOCALAPPDATA%\Famo\famo-settings.json`(契约见 `famo-settings.schema.json`)，
> 由配置写入层翻译成下列 RIME/Weasel 文件的具体键，再按"桶"决定生效动作。
> 两档生效模型见 PRD §2.3。字段与设计稿 `design/settings/FamoWindowsIME.html` §3 一一对应。

桶 / 生效动作：
- **即时(instant) → reload**：写 `weasel.custom.yaml`，触发 Weasel 轻量 reload(零点几秒)，候选窗重绘，**不**走 WeaselDeployer 重部署。
- **部署(deploy) → deploy**：写 RIME yaml，调 `FamoRuntime.exe --control deploy` 经独立控制队列重编译 prism/词库；维护期间按键立即 fail-open。

---

## ① 输入方式（设计稿「输入方式」页，部署桶）

| store 字段 | 目标文件 | 键 | 桶 | 动作 |
|---|---|---|---|---|
| `engine.schemaList[]`（enabled+顺序，首个 enabled=默认） | `default.custom.yaml` | `schema_list`（只写 enabled=true 项，按数组序） | 部署 | deploy |
| `engine.pageSize` | `default.custom.yaml` | `menu/page_size`（出厂 8；rime-ice 上游默认 5） | 部署 | deploy |

方案 id ↔ 上游方案文件（雾凇拼音优先，首项 rime_ice）：

| id | 方案 | 来源 |
|---|---|---|
| `rime_ice` | 雾凇拼音·全拼（默认） | rime-ice `rime_ice.schema.yaml` |
| `wubi86_jidian` | 极点五笔 86 | KyleBing/rime-wubi86-jidian overlay `wubi86_jidian.schema.yaml` |
| `double_pinyin_flypy` | 小鹤双拼 | rime-ice `double_pinyin_flypy.schema.yaml` |
| `t9` | 中文九键 | rime-ice `t9.schema.yaml`（依赖 rime_ice） |
| `wubi86_jidian_pinyin` | 极点五笔·拼音混输 | KyleBing overlay `wubi86_jidian_pinyin.schema.yaml`，法墨打包时注入 `zh_trad` 繁体开关 |
| `wubi86_jidian_trad` | 极点五笔·简入繁出 | KyleBing overlay `wubi86_jidian_trad.schema.yaml` |
| `wubi86_jidian_trad_pinyin` | 极点五笔·拼音混输·简入繁出 | KyleBing overlay `wubi86_jidian_trad_pinyin.schema.yaml` |
| `double_pinyin` / `_mspy` / `_sogou` / `_abc` / `_ziguang` / `_jiajia` | 自然码/微软/搜狗/智能ABC/紫光/拼音加加 双拼 | rime-ice `double_pinyin*.schema.yaml` |

> 底座 = **iDvel/rime-ice 雾凇（GPL-3.0）** 全套拼音方案 + **KyleBing/rime-wubi86-jidian（Apache-2.0）** 五笔 4 变体（自带 pinyin_simp/numbers 反查依赖，自洽）。见 `assemble-payload.sh`。
> `pinyin_simp` 仅作五笔反查词典存在，不在用户可见 `schema_list`（rime_ice 已是拼音主方案）。

> 注：rime-ice 各 `*.schema.yaml` 可能自带 `menu/page_size`，会就近覆盖 `default.custom.yaml/menu/page_size` 全局值——
> 若某方案候选数没跟随，需在该 schema 的 custom 里同步(本切片暂不处理)。

---

## ② 候选皮肤 / 配色（设计稿「皮肤外观」页，即时桶）

| store 字段 | 目标文件 | 键 | 桶 | 动作 |
|---|---|---|---|---|
| `appearance.skin` | `weasel.custom.yaml` | `style/color_scheme`（亮色用 `<skin>`） | 即时 | reload |
| `appearance.appearanceMode` | `weasel.custom.yaml` | `style/color_scheme` ↔ `style/color_scheme_dark`（system=亮/暗自动；light=`<skin>`；dark=`<skin>_dark`） | 即时 | reload |

四款皮肤(配色块在 `weasel.custom.yaml` 的 `preset_color_schemes/*`；色值为 **Weasel BGR `0xBBGGRR`**，与 mac 端同一真相源 `shared/skins/colors.md`)：

| skin | color_scheme（亮 / 暗） | 中文名 | 品牌 accent（亮 / 暗，原 RGB） |
|---|---|---|---|
| `shenda`（默认） | `shenda` / `shenda_dark` | 荔园红 | #A82C53 / #E06A8E（store 里 BGR：`hilited_candidate_back_color` 亮 `0x532CA8` / 暗 `0x8E6AE0`） |
| `stanford` | `stanford` / `stanford_dark` | 胡佛红 | #8C1515 / #B83A4B（BGR 亮 `0x15158C` / 暗 `0x4B3AB8`） |
| `wuda` | `wuda` / `wuda_dark` | 珞珈青 | #2A8367 / #3CA081（BGR 亮 `0x67832A` / 暗 `0x81A03C`） |
| `xiada` | `xiada` / `xiada_dark` | 嘉庚蓝 | #1D4A8C / #4879C5（BGR 亮 `0x8C4A1D` / 暗 `0xC57948`） |

> `preset_color_schemes/*` 的全部色值随 `weasel.custom.yaml` 打包(assemble-payload.sh 落地)，设置面板只切 `color_scheme`，不逐项改色(皮肤自定义编辑器 v1 WON'T)。

---

## ③ 候选窗外观（设计稿「候选外观」页，全即时桶）

| store 字段 | 目标文件 | 键 | 桶 | 动作 |
|---|---|---|---|---|
| `appearance.fontFace` | `weasel.custom.yaml` | `style/font_face`（+ 可选 `label_font_face`/`comment_font_face`） | 即时 | reload |
| `appearance.fontPoint` | `weasel.custom.yaml` | `style/font_point` | 即时 | reload |
| `appearance.orientation` | `weasel.custom.yaml` | `style/horizontal`（horizontal→true / vertical→false） | 即时 | reload |
| `appearance.inlinePreedit` | `famo-style.yaml` | `style/inline_preedit` | 即时 | `--control reload-style` → native TSF host |
| `appearance.showPreedit` | `famo-style.yaml` | `style/show_preedit`（候选窗输入串 + 光标，独立于 inline_preedit） | 即时 | `--control reload-style` → candidate layout |
| `appearance.previewPages` | `famo-style.yaml` | `style/preview_pages` | 即时 | `--control reload-style` → librime 只读候选迭代 |
| `appearance.previewRows` | `famo-style.yaml` | `style/preview_rows`（1–2） | 即时 | `--control reload-style` → candidate layout |
| `appearance.inlineCandidatePreview` | `famo-style.yaml` | `style/preedit_type`（composition/preview） | 即时 | `--control reload-style` → native TSF host |
| `appearance.layout.cornerRadius` | `weasel.custom.yaml` | `style/layout/corner_radius` | 即时 | reload |
| `appearance.layout.borderWidth` | `weasel.custom.yaml` | `style/layout/border_width` | 即时 | reload |
| `appearance.layout.shadowRadius` | `weasel.custom.yaml` | `style/layout/shadow_radius` | 即时 | reload |
| `appearance.layout.margin` | `weasel.custom.yaml` | `style/layout/margin_x` + `style/layout/margin_y`（同值） | 即时 | reload |

---

## ④ 输入行为（设计稿「输入行为」页，部署桶）

| store 字段 | 目标文件 | 键 | 桶 | 动作 |
|---|---|---|---|---|
| `engine.fuzzyPinyin.zh_z` | `rime_ice.custom.yaml` | `speller/algebra/+`（追加 `derive/^zh/z/`） | 部署 | deploy |
| `engine.fuzzyPinyin.ch_c` | `rime_ice.custom.yaml` | `speller/algebra/+`（追加 `derive/^ch/c/`） | 部署 | deploy |
| `engine.fuzzyPinyin.sh_s` | `rime_ice.custom.yaml` | `speller/algebra/+`（追加 `derive/^sh/s/`） | 部署 | deploy |
| `engine.fuzzyPinyin.n_l` | `rime_ice.custom.yaml` | `speller/algebra/+`（双向：`derive/^n/l/` `derive/^l/n/`） | 部署 | deploy |
| `engine.fuzzyPinyin.r_l` | `rime_ice.custom.yaml` | `speller/algebra/+`（追加 `derive/^r/l/`） | 部署 | deploy |
| `engine.fuzzyPinyin.f_h` | `rime_ice.custom.yaml` | `speller/algebra/+`（双向：`derive/^f/h/` `derive/^h/f/`） | 部署 | deploy |
| `engine.fuzzyPinyin.an_ang` | `rime_ice.custom.yaml` | `speller/algebra/+`（双向：`derive/an$/ang/` `derive/ang$/an/`） | 部署 | deploy |
| `engine.fuzzyPinyin.en_eng` | `rime_ice.custom.yaml` | `speller/algebra/+`（双向：`derive/en$/eng/` `derive/eng$/en/`） | 部署 | deploy |
| `engine.fuzzyPinyin.in_ing` | `rime_ice.custom.yaml` | `speller/algebra/+`（双向：`derive/in$/ing/` `derive/ing$/in/`） | 部署 | deploy |
| `engine.emojiEnabled` | `rime_ice.custom.yaml` | `switches` 中 `emoji/reset`（出厂 0=关；rime-ice 默认 1） | 部署 | deploy |
| `engine.wubi.codeHint` | `wubi86_jidian.custom.yaml` | `translator/comment_format`（on=`[]` 留原始编码；off=`- xform/.+//` 抹注释） | 部署 | deploy |
| `engine.wubi.autoClear` | `wubi86_jidian.custom.yaml` | `speller/auto_clear`（on=`max_length`；off=`""`） | 部署 | deploy |
| `engine.wubi.candidateMode` | `wubi86_jidian.custom.yaml` | `engine/filters/+`（single_first/only 追 `lua_filter@*wubi86_jidian_single_char_*`；normal 不发） | 部署 | deploy |
| `engine.wubi.zReverseLookup` | `wubi86_jidian.custom.yaml` | `recognizer/patterns/reverse_lookup`（off=`""` 清 z 反查；on 不发） | 部署 | deploy |

> emoji 同时写即时桶 `famo-options.yaml` `options/emoji`（set_option 即时切）；部署桶 reset 为出厂兜底。
> 默认输入态/中英标点/全半角/简繁（`ascii_mode`/`ascii_punct`/`full_shape`/`traditionalization`）= 即时开关桶
> `famo-options.yaml`，零部署；经 AddSession 回放 = 每个新会话默认态（对位 mac 默认状态）。
> 简繁是跨方案语义按钮：同时发 `traditionalization`(rime-ice/双拼/t9) + `zh_trad`(五笔)，繁体默认覆盖各方案（set_option 到无此 option 为空操作）。
> 任何进入用户可选 `schema_list` 的方案都必须暴露这两条 option 之一；`wubi86_jidian_pinyin` 上游简体混输版没有该开关，
> 因此 `assemble-payload.sh` 会在打包时补入 `zh_trad` + `simplifier@tradition`，避免“繁体”按钮只改变 UI 状态。
> 五笔专属：写 `wubi86_jidian.custom.yaml`（base 合并 + `famo-wubi` 标记，保出厂 schema/icon 品牌图标、幂等）；
> 逐字段镜像 mac `famoWubiCustomYAML`；单字 lua 依赖随包 `payload/lua/wubi86_jidian_single_char_*.lua`。
> Windows 只写 `wubi86_jidian.custom.yaml`（picker 选「五笔」只到此方案，繁体走其上 `zh_trad` option）。
> tone_display（带声调 preedit）已下线：rime-ice 无此开关，用户确认无用。
> 模糊音：9 独立对，`/+` = 追加到既有 algebra 列表末尾，逐对 gated 只追加用户勾选项（锚定 `^`/`$`；
> `n_l`/`f_h`/`an_ang`/`en_eng`/`in_ing` 双向各发两条；逐字段镜像 mac `FamoRimePatchBuilder.fuzzyAlgebraDerives`）。
> 旧 3 组合(`zh_ch_sh`/`an_en_in`/`l_n_f_h_r_l`)由 `SettingsStore.Load` 迁移到这 9 项。
> 双拼方案 algebra 与全拼不同，**不能套全拼**——双拼模糊音需各自 `*.custom.yaml`(本切片不展开)。
> `switches` 为 list，RIME custom patch 不支持按名打单条 reset；assemble-payload.sh 在
> `rime_ice.custom.yaml` 整体 patch `switches:`，镜像 rime-ice 列表但仅翻转 emoji 一条(见该文件注释)。

---

## ⑤ 快捷键与输入便利（设计稿「快捷键设置 / 输入便利」页，部署桶）

| store 字段 | 目标文件 | 键 | 桶 | 动作 |
|---|---|---|---|---|
| `convenience.shiftSwitch` | `default.custom.yaml` | `ascii_composer/switch_key/Shift_L` + `Shift_R`（on=`commit_code`，off=`noop`） | 部署 | deploy |
| `convenience.goodOldCapsLock` | `default.custom.yaml` | `ascii_composer/good_old_caps_lock`（on=`true`，off=`false`） | 部署 | deploy |
| `convenience.pageMinusEquals` | `default.custom.yaml` | `key_binder/bindings` 中 `minus/equal → Page_Up/Page_Down`（off 时从托管列表移除） | 部署 | deploy |
| `convenience.pageBrackets` | `default.custom.yaml` | `key_binder/select_first_character` + `select_last_character` 置空，并绑定 `bracketleft/right → Page_Up/Page_Down` | 部署 | deploy |
| `convenience.pageCommaPeriod` | `default.custom.yaml` | `key_binder/bindings` 中 `comma/period → Page_Up/Page_Down` | 部署 | deploy |
| `convenience.select23Semicolon` | `default.custom.yaml` | `key_binder/bindings` 中 `semicolon/apostrophe → 2/3` | 部署 | deploy |
| `convenience.slashToDun` | `default.custom.yaml` | `punctuator/half_shape/+` + `punctuator/full_shape/+` 的 `/ → 、` | 部署 | deploy |
| `convenience.digitSeparators` | `default.custom.yaml` | `punctuator/digit_separators: ",.:"` | 部署 | deploy |
| `convenience.autoPairPunctuation` | `famo-style.yaml` | `style/famo_auto_pair`（默认 false） | 即时 | `--control reload-style` → native TSF edit session |
| `convenience.cjkEnglishSpacing` | `famo-style.yaml` | `style/famo_cjk_english_spacing`（默认 false） | 即时 | `--control reload-style` → native TSF edit session |
| `convenience.cjkNumberSpacing` | `famo-style.yaml` | `style/famo_cjk_number_spacing`（默认 false） | 即时 | `--control reload-style` → native TSF edit session |
| `convenience.appEnglishExes[]` | `default.custom.yaml` | `app_options/<exe>/ascii_mode: true` | 部署 | deploy |

> `default.custom.yaml` 由设置面板整文件生成。`key_binder/bindings` 是法墨托管列表：保留 rime-ice 当前启用的
> Tab/Alt 光标移动与中英标点热键；这样 `-`/`=` 翻页关闭时不会被底座默认绑定继续生效。
> 简繁热键暂不生成全局 `Control+Shift+4`：rime-ice 用 `traditionalization`，五笔用 `zh_trad`，
> RIME key_binder 的单个 `toggle` 不能像状态栏/菜单那样同时 fan-out 多个 option。没有跨当前方案验证前，
> 不允许暴露只对某个方案族生效的全局快捷键。

---

## 文件落点总览（`%LOCALAPPDATA%\Famo`）

| 文件 | 写者 | 角色 |
|---|---|---|
| `famo-settings.json` | 设置面板 | store 单一真相源(本契约) |
| `weasel.custom.yaml` | 配置写入层(即时桶) | 候选窗外观/皮肤 → reload |
| `default.custom.yaml` | 配置写入层(部署桶) | 方案列表/候选数 → deploy |
| `rime_ice.custom.yaml` | 配置写入层(部署桶) | 模糊音/emoji 出厂锚点 + 拼音快捷短语 translator → deploy |
| `wubi86_jidian.custom.yaml` | 配置写入层(部署桶) | 五笔专属（码提示/空码清码/单字候选/z反查）；不注入字母快捷短语 translator |
| `famo-options.yaml` | 配置写入层(即时开关桶) | 默认输入态/中英标点/全半角/简繁(+zh_trad)/emoji → set_option 即时 |
| `quick-phrases.json` | 快捷短语页 | 编辑态 store，编码唯一、保存时规范化 |
| `famo_quick_send.txt` | 快捷短语页 | RIME tabledb，`db_name=famo_quick_send`；编辑态与派生表都用裸码，关闭补全后仅完整编码命中；五笔用 `quick-phrase-picker` 显式面板插入 |

> 形态甲：以上全部落 `%LOCALAPPDATA%\Famo`，**不碰** `%AppData%\Rime`(ADR-0004)。
