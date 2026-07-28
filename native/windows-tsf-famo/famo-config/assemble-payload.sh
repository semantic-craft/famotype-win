#!/usr/bin/env bash
#
# assemble-payload.sh — 组装法墨 Windows 配置 payload
# ============================================================================
# 把 rime-ice（雾凇拼音）完整配置 + KyleBing 极点五笔 + 法墨 overlay 组装到
#   native/windows-tsf-famo/famo-config/payload/
# 这份 payload 即首启时 seed 到 %LOCALAPPDATA%\Famo 的内置配置包。
#
# 底座来源与许可证（随包须列 THIRD-PARTY-NOTICES，见 PRD §6.3）：
#   - iDvel/rime-ice        : 雾凇拼音全套(含 cn_dicts/ en_dicts/ lua/ opencc/ 双拼/melt_eng/
#                             t9/custom_phrase)。GPL-3.0。词库源含腾讯词库等，详见随包 NOTICES。
#   - KyleBing/rime-wubi86-jidian : 极点五笔 86 全套 overlay。Apache-2.0。
#                             带入 4 个 wubi 变体 (wubi86_jidian / _pinyin / _trad / _trad_pinyin)、
#                             其反查依赖 pinyin_simp(.schema/.dict)、numbers.schema、各 wubi dict、
#                             KyleBing lua（五笔反查自洽，不依赖 rime-ice）。无法学五笔。
#   - BYVoid/OpenCC 1.1.9   : rime-ice/五笔简繁转换所需的标准配置和 ocd2 词典。Apache-2.0。
#   - 法墨 overlay/         : 本仓 default.custom.yaml / weasel.custom.yaml / rime_ice.custom.yaml。GPLv3。
#
# lua 解析：rime-ice 与 KyleBing 均走 librime 的 `*函数名` 约定（lua_translator@*name 自动
#   加载 lua/name.lua），无中央 rime.lua。KyleBing 的 wubi lua 拷进 payload/lua/ 即自动解析。
#
# 不带入法学层：上游本身不含 famo_law_* / wubi86_law / law_*.lua；
#   脚本另做防御式剔除，确保即使上游或缓存夹带也不进 payload(见 LAW_PATTERNS)。
#
# 幂等：每次重建 payload/(先清后填)。失败即非零退出，不假装成功。
# ============================================================================
set -euo pipefail

# ── 路径 ────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OVERLAY_DIR="${SCRIPT_DIR}/overlay"
PAYLOAD_DIR="${SCRIPT_DIR}/payload"
CACHE_DIR="${SCRIPT_DIR}/.cache"
ICE_DIR="${CACHE_DIR}/rime-ice"
WUBI_DIR="${CACHE_DIR}/rime-wubi86-jidian"
OPENCC_STANDARD_DIR="${SCRIPT_DIR}/opencc-standard"

ICE_REPO="https://github.com/iDvel/rime-ice.git"
ICE_REF_PINNED=0
if [ -n "${ICE_REF:-}" ]; then
  ICE_REF_PINNED=1
fi
ICE_REF="${ICE_REF:-main}"   # 可用环境变量钉住特定 tag/commit
WUBI_REPO="https://github.com/KyleBing/rime-wubi86-jidian.git"
WUBI_REF_PINNED=0
if [ -n "${WUBI_REF:-}" ]; then
  WUBI_REF_PINNED=1
fi
WUBI_REF="${WUBI_REF:-master}"

# 法学层防御式剔除模式(本应为空集，纯保险)。
LAW_PATTERNS=( 'famo_law_*' 'wubi86_law*' 'law_*.lua' 'law_phrase*' )

log()  { printf '\033[1;32m[assemble]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[assemble]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[assemble] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ── 0. 前置检查 ─────────────────────────────────────────────────────────────
command -v git >/dev/null 2>&1 || die "未找到 git，请先安装 git。"

ensure_canonical_origin() {
  local name="$1" repo="$2" dir="$3"
  if git -C "${dir}" remote | grep -Fxq origin; then
    git -C "${dir}" remote set-url origin "${repo}" \
      || die "无法修正 ${name} origin：${repo}"
  else
    git -C "${dir}" remote add origin "${repo}" \
      || die "无法配置 ${name} remote：${repo}"
  fi
}

clean_managed_repo() {
  local name="$1" dir="$2"
  git -C "${dir}" clean -ffdx \
    || die "无法清理 ${name} 缓存中的未跟踪/忽略文件：${dir}"
}

checkout_pinned_ref() {
  local name="$1" repo="$2" ref="$3" dir="$4"
  if [ -d "${dir}/.git" ]; then
    log "更新缓存 ${name} (${dir}) 到显式 ref ${ref} …"
  else
    log "初始化 ${name} (${repo}@${ref}) → ${dir} …"
    rm -rf "${dir}"
    git init --quiet "${dir}" || die "无法初始化 ${name} 缓存：${dir}"
  fi

  ensure_canonical_origin "${name}" "${repo}" "${dir}"
  git -C "${dir}" fetch --depth 1 origin "${ref}" \
    || die "无法解析或 fetch ${name} 显式 ref：${ref}"
  git -C "${dir}" reset --hard FETCH_HEAD \
    || die "无法检出 ${name} 显式 ref：${ref}"

  local expected_head actual_head
  expected_head="$(git -C "${dir}" rev-parse --verify 'FETCH_HEAD^{commit}')"
  actual_head="$(git -C "${dir}" rev-parse --verify HEAD)"
  [ "${actual_head}" = "${expected_head}" ] \
    || die "${name} 未检出显式 ref ${ref}（HEAD=${actual_head}，期望=${expected_head}）"
  clean_managed_repo "${name}" "${dir}"
  log "${name} HEAD = $(git -C "${dir}" rev-parse --short HEAD)"
}

# ── 1. 拉取 / 更新 rime-ice（幂等；网络不可用则优雅报错，回退已缓存 .cache）──────
fetch_rime_ice() {
  mkdir -p "${CACHE_DIR}"
  if [ "${ICE_REF_PINNED}" -eq 1 ]; then
    checkout_pinned_ref "rime-ice" "${ICE_REPO}" "${ICE_REF}" "${ICE_DIR}"
    return
  fi

  if [ -d "${ICE_DIR}/.git" ]; then
    log "更新缓存 rime-ice (${ICE_DIR}) …"
    ensure_canonical_origin "rime-ice" "${ICE_REPO}" "${ICE_DIR}"
    if git -C "${ICE_DIR}" fetch --depth 1 origin "${ICE_REF}"; then
      git -C "${ICE_DIR}" reset --hard FETCH_HEAD
    else
      warn "无法 fetch rime-ice（网络/代理问题？）。沿用已缓存的 ${ICE_DIR}（离线模式）。"
    fi
  else
    log "克隆 rime-ice (${ICE_REPO}@${ICE_REF}) → ${ICE_DIR} …"
    rm -rf "${ICE_DIR}"
    if ! git clone --depth 1 --branch "${ICE_REF}" "${ICE_REPO}" "${ICE_DIR}" 2>/dev/null; then
      # branch 形式失败时退回默认分支克隆(ICE_REF 可能是 commit)
      git clone --depth 1 "${ICE_REPO}" "${ICE_DIR}" \
        || die "克隆 rime-ice 失败（网络不可用？）。手动步骤：
        1) 在能联网处执行：git clone ${ICE_REPO} ${ICE_DIR}
        2) 重跑本脚本(检测到 .cache/rime-ice/.git 即走更新分支)。"
    fi
  fi
  clean_managed_repo "rime-ice" "${ICE_DIR}"
  log "rime-ice HEAD = $(git -C "${ICE_DIR}" rev-parse --short HEAD)"
}

# ── 1b. 拉取 / 更新 KyleBing 极点五笔（Apache-2.0；幂等）───────────────────────
fetch_wubi() {
  mkdir -p "${CACHE_DIR}"
  if [ "${WUBI_REF_PINNED}" -eq 1 ]; then
    checkout_pinned_ref "rime-wubi86-jidian" "${WUBI_REPO}" "${WUBI_REF}" "${WUBI_DIR}"
    return
  fi

  if [ -d "${WUBI_DIR}/.git" ]; then
    log "更新缓存 rime-wubi86-jidian (${WUBI_DIR}) …"
    ensure_canonical_origin "rime-wubi86-jidian" "${WUBI_REPO}" "${WUBI_DIR}"
    if git -C "${WUBI_DIR}" fetch --depth 1 origin "${WUBI_REF}"; then
      git -C "${WUBI_DIR}" reset --hard FETCH_HEAD
    else
      warn "无法 fetch KyleBing/rime-wubi86-jidian。沿用已缓存的 ${WUBI_DIR}（离线模式）。"
    fi
  else
    log "克隆 rime-wubi86-jidian (${WUBI_REPO}@${WUBI_REF}) → ${WUBI_DIR} …"
    rm -rf "${WUBI_DIR}"
    if ! git clone --depth 1 --branch "${WUBI_REF}" "${WUBI_REPO}" "${WUBI_DIR}" 2>/dev/null; then
      git clone --depth 1 "${WUBI_REPO}" "${WUBI_DIR}" \
        || die "克隆 KyleBing/rime-wubi86-jidian 失败（网络不可用？）。手动：
        1) git clone ${WUBI_REPO} ${WUBI_DIR}
        2) 重跑本脚本。"
    fi
  fi
  clean_managed_repo "rime-wubi86-jidian" "${WUBI_DIR}"
  log "rime-wubi86-jidian HEAD = $(git -C "${WUBI_DIR}" rev-parse --short HEAD)"
}

# ── 2. 把 rime-ice 全套复制进 payload（排除 .git / 平台无关项 / 预编译产物）──────
copy_base() {
  log "重建 payload 目录：${PAYLOAD_DIR}"
  rm -rf "${PAYLOAD_DIR}"
  mkdir -p "${PAYLOAD_DIR}"

  # 复制 rime-ice 全树(含 cn_dicts/ en_dicts/ lua/ opencc/ 各 schema/dict、custom_phrase、symbols)。
  # 排除：VCS、文档、预编译 build/、非 Windows 前端配置、recipe/others 研发资料。
  ( cd "${ICE_DIR}" && \
    tar --exclude='.git' --exclude='.github' --exclude='*.md' \
        --exclude='build' --exclude='recipe.yaml' --exclude='squirrel.yaml' \
        --exclude='preview' --exclude='tools' --exclude='plum' --exclude='others' \
        -cf - . ) | ( cd "${PAYLOAD_DIR}" && tar -xf - )

  log "rime-ice 基座已落地($(find "${PAYLOAD_DIR}" -type f | wc -l | tr -d ' ') 文件)。"
}

reject_payload_links() {
  local link
  link="$(find "${PAYLOAD_DIR}" -type l -print -quit)"
  [ -z "${link}" ] \
    || die "拒绝 payload 符号链接（Windows 发布包只允许普通文件/目录）：${link#${PAYLOAD_DIR}/}"
}

# rime-ice 只携带 emoji OpenCC 数据；s2t/s2hk 是前端通常预装的标准数据。
# 法墨使用自足 data_root，必须显式带入，否则开关会变成“状态已繁、候选仍简”。
overlay_opencc_standard() {
  local opencc_dir="${PAYLOAD_DIR}/opencc"
  [ ! -L "${opencc_dir}" ] || die "拒绝 OpenCC 符号链接目录"
  mkdir -p -- "${opencc_dir}"
  [ -d "${opencc_dir}" ] || die "OpenCC 目标不是目录"
  for f in s2t.json s2hk.json STCharacters.ocd2 STPhrases.ocd2 HKVariants.ocd2 LICENSE; do
    [ -f "${OPENCC_STANDARD_DIR}/${f}" ] || die "缺少 OpenCC 标准数据：opencc-standard/${f}"
    local name="${f}"
    if [ "${f}" = LICENSE ]; then
      name=OpenCC.LICENSE
    fi
    local destination="${opencc_dir}/${name}"
    rm -f -- "${destination}"
    cp -- "${OPENCC_STANDARD_DIR}/${f}" "${destination}"
  done
  log "OpenCC 标准简繁数据已叠加（s2t + s2hk）。"
}

# ── 3. 防御式剔除法学层(应为空操作)───────────────────────────────────────────
strip_law_layer() {
  local removed=0
  for pat in "${LAW_PATTERNS[@]}"; do
    while IFS= read -r -d '' f; do
      warn "剔除法学层残留：${f#${PAYLOAD_DIR}/}"
      rm -f "${f}"; removed=$((removed+1))
    done < <(find "${PAYLOAD_DIR}" -type f -name "${pat}" -print0 2>/dev/null)
  done
  [ "${removed}" -eq 0 ] && log "无法学层残留(符合预期)。" || warn "共剔除 ${removed} 个法学层文件。"
}

# ── 3b. KyleBing 五笔 overlay（Apache-2.0）—— 必须在 rime-ice base 之后 ─────────
# KyleBing 带入 4 个 wubi 变体 + 反查依赖 pinyin_simp（schema/dict）+ numbers schema + 各 wubi dict
# + KyleBing lua（五笔反查自洽，不依赖 rime-ice）。
# 只拷 *.schema.yaml *.dict.yaml lua/*.lua LICENSE —— 不拷 KyleBing 的 *.custom.yaml（会覆盖 Famo 配置）。
overlay_wubi() {
  mkdir -p "${PAYLOAD_DIR}/lua"
  cp "${WUBI_DIR}"/*.schema.yaml "${WUBI_DIR}"/*.dict.yaml "${PAYLOAD_DIR}/"
  cp "${WUBI_DIR}"/lua/*.lua "${PAYLOAD_DIR}/lua/" 2>/dev/null || true
  cp "${WUBI_DIR}/LICENSE" "${PAYLOAD_DIR}/wubi86-jidian.LICENSE"
  # 必修 bug：KyleBing 的 _trad / _trad_pinyin schema 里 active 的 `lua_translator@date_translator`
  # 是短名（无 `*` 前缀），librime 无法解析 → 改成 `*wubi86_jidian_date_translator`（`*` 前缀自动
  # 加载 lua/wubi86_jidian_date_translator.lua，该文件随 KyleBing lua 一并落地）。
  for trad in wubi86_jidian_trad.schema.yaml wubi86_jidian_trad_pinyin.schema.yaml; do
    if [ -f "${PAYLOAD_DIR}/${trad}" ]; then
      sed -i 's/^\(  *\)- lua_translator@date_translator/\1- lua_translator@*wubi86_jidian_date_translator/' \
        "${PAYLOAD_DIR}/${trad}"
    fi
  done
  log "KyleBing 五笔 overlay 叠加（4 变体 + pinyin_simp + numbers + lua + LICENSE）。"
}

# 上游 wubi86_jidian_pinyin 是简体混输版，本身没有 zh_trad option；
# 但法墨 Windows 将它列为用户可选方案，所以“简/繁”按钮必须能真实作用到它。
ensure_wubi_pinyin_traditionalization() {
  local schema="${PAYLOAD_DIR}/wubi86_jidian_pinyin.schema.yaml"
  [ -f "${schema}" ] || die "缺少 wubi86_jidian_pinyin.schema.yaml，无法注入繁体开关。"

  if ! grep -q 'name: zh_trad' "${schema}"; then
    sed -i '/^switches:$/a\
  - name: zh_trad\
    reset: 0    # 初始状态为 0: 简体 1: 繁体\
    states: [ 简体, 繁体 ]' "${schema}"
  fi

  if ! grep -q 'simplifier@tradition' "${schema}"; then
    sed -i '/^    - table_translator$/a\
  filters:\
    - simplifier@tradition\
    - uniquifier' "${schema}"
  fi

  if ! grep -q 'option_name: zh_trad' "${schema}"; then
    cat >> "${schema}" <<'EOF'

# 法墨补丁：上游拼音混输简体版没有简繁开关，但它在 Windows 设置中是可选方案。
# 与极点五笔普通/繁体变体保持一致，让状态栏/菜单的“繁”按钮设置 zh_trad 后真实生效。
tradition:
  opencc_config: s2hk.json
  option_name: zh_trad
EOF
  fi

  log "极点五笔拼音混输 schema 已注入 zh_trad 繁体开关。"
}

# ── 4. 叠加法墨 overlay（schema_list / page_size / 四皮肤 / emoji-off / 品牌图标）──
apply_overlay() {
  for f in default.custom.yaml weasel.custom.yaml rime_ice.custom.yaml; do
    [ -f "${OVERLAY_DIR}/${f}" ] || die "缺少 overlay 源文件：overlay/${f}"
    cp "${OVERLAY_DIR}/${f}" "${PAYLOAD_DIR}/${f}"
    log "overlay 叠加：${f}"
  done
  # 法墨品牌托盘/状态图标（墨滴）。schema/icon 等键按文件名引用，须随 payload seed 到 Famo 目录。
  for ico in "${OVERLAY_DIR}"/*.ico; do
    [ -e "${ico}" ] || continue
    cp "${ico}" "${PAYLOAD_DIR}/"
    log "品牌图标叠加：$(basename "${ico}")"
  done
}

# ── 4b. 品牌托盘图标：给 schema_list 里每个方案生成 icon-only custom，统一显示法墨墨滴 ──
# Weasel schema/icon 是 per-schema 配置；为让任意激活方案都显示墨滴（而非内置"中"），
# 给每个方案打 custom patch 覆盖其自带 icon（如 wubi86_jidian 自带但 .ico 缺失→回退中）。
# rime_ice 的图标已写在 overlay/rime_ice.custom.yaml，此处跳过。
brand_schema_icons() {
  local zh="famo_zh.ico" en="famo_ascii.ico" n=0
  local ids
  ids=$(grep -E '^[[:space:]]*- schema:' "${OVERLAY_DIR}/default.custom.yaml" \
        | sed -E 's/.*- schema:[[:space:]]*([A-Za-z0-9_]+).*/\1/')
  for id in ${ids}; do
    [ "${id}" = "rime_ice" ] && continue
    local cf="${PAYLOAD_DIR}/${id}.custom.yaml"
    if [ -f "${cf}" ] && grep -q 'schema/icon' "${cf}"; then continue; fi
    if [ -f "${cf}" ]; then
      warn "已有 ${id}.custom.yaml（含其它 patch），跳过图标注入——如需请手动合并 schema/icon。"
      continue
    fi
    cat > "${cf}" <<EOF
# ${id}.custom.yaml — 法墨品牌托盘图标补丁（assemble-payload.sh 生成，请勿手改）。
# schema/icon 等键覆盖方案自带图标，统一显示法墨墨滴（与 macOS 版 Famo 对齐）。
patch:
  "schema/icon": ${zh}
  "schema/ascii_icon": ${en}
  "schema/full_icon": ${zh}
  "schema/half_icon": ${zh}
EOF
    n=$((n + 1))
  done
  log "品牌图标 custom 生成：${n} 个方案（rime_ice 已含于 overlay）。"
}

neutralize_payload_identity_text() {
  local cleaned=0
  while IFS= read -r -d '' f; do
    if grep -Eq '#.*([Ww]easel|小狼毫)' "${f}"; then
      sed -i -E '/^[[:space:]]*#.*[Ww]easel/d;/^[[:space:]]*#.*小狼毫/d;/#.*[Ww]easel/s/[[:space:]]*#.*$//;/#.*小狼毫/s/[[:space:]]*#.*$//' "${f}"
      cleaned=$((cleaned + 1))
    fi
  done < <(find "${PAYLOAD_DIR}" -type f \( -name '*.yaml' -o -name '*.yml' \) ! -name '*.dict.yaml' -print0)
  log "发布 payload 旧前端注释清理：${cleaned} 个 YAML 文件。"
}

# ── 5. 体检：关键文件 + 出厂锚定就位 + 旧底座/限制残留断言 ──────────────────────
verify() {
  local ok=1
  reject_payload_links
  for f in rime_ice.schema.yaml rime_ice.dict.yaml \
           t9.schema.yaml melt_eng.schema.yaml \
           double_pinyin_flypy.schema.yaml \
           cn_dicts/base.dict.yaml \
           wubi86_jidian.schema.yaml wubi86_jidian.dict.yaml \
           wubi86_jidian_pinyin.schema.yaml wubi86_jidian_trad.schema.yaml \
           wubi86_jidian_trad_pinyin.schema.yaml \
           pinyin_simp.dict.yaml numbers.schema.yaml \
           lua/wubi86_jidian_date_translator.lua \
           wubi86-jidian.LICENSE \
           opencc/s2t.json opencc/s2hk.json \
           opencc/STCharacters.ocd2 opencc/STPhrases.ocd2 \
           opencc/HKVariants.ocd2 opencc/OpenCC.LICENSE \
           default.custom.yaml weasel.custom.yaml rime_ice.custom.yaml; do
    if [ -f "${PAYLOAD_DIR}/${f}" ]; then
      log "  ✓ ${f}"
    else
      warn "  ✗ 缺失：${f}"; ok=0
    fi
  done
  # 出厂锚定抽检
  grep -q 'color_scheme: shenda' "${PAYLOAD_DIR}/weasel.custom.yaml"  || { warn "默认皮肤非 shenda"; ok=0; }
  grep -q 'page_size: 8'         "${PAYLOAD_DIR}/default.custom.yaml" || { warn "page_size 非 8"; ok=0; }
  grep -q 'schema: rime_ice'     "${PAYLOAD_DIR}/default.custom.yaml" || { warn "schema_list 无 rime_ice"; ok=0; }
  # 旧底座/奇葩限制残留断言（迁移红线）。去掉注释(# 之后)再判，避免误伤 overlay 里的说明性注释。
  if grep -rhn 'codeLengthLimit' "${PAYLOAD_DIR}" 2>/dev/null | sed 's/#.*//' | grep -q 'codeLengthLimit'; then
    warn "payload 残留 codeLengthLimit（长句限制必须随 oh-my-rime 一并消失）"; ok=0
  fi
  if ls "${PAYLOAD_DIR}"/rime_mint.* >/dev/null 2>&1; then
    warn "payload 残留 rime_mint.*（旧薄荷底座未清干净）"; ok=0
  fi
  [ "${ok}" -eq 1 ] || die "payload 体检未通过。"
  log "payload 体检通过 → ${PAYLOAD_DIR}"
}

main() {
  fetch_rime_ice
  fetch_wubi
  copy_base
  reject_payload_links
  overlay_opencc_standard
  overlay_wubi
  ensure_wubi_pinyin_traditionalization
  strip_law_layer
  apply_overlay
  brand_schema_icons
  neutralize_payload_identity_text
  verify
  log "完成。payload 可作为首启 seed 到 %LOCALAPPDATA%\\Famo 的内置配置包。"
}

main "$@"
