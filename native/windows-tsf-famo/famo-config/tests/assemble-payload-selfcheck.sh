#!/usr/bin/env bash
set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$(cd "${TEST_DIR}/.." && pwd)"
ASSEMBLE_SCRIPT="${FAMO_TEST_ASSEMBLE_SCRIPT:-${CONFIG_DIR}/assemble-payload.sh}"
ICE_REPO_URL="https://github.com/iDvel/rime-ice.git"
WUBI_REPO_URL="https://github.com/KyleBing/rime-wubi86-jidian.git"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/famo-assemble-selfcheck.XXXXXX")"

cleanup() {
  rm -rf -- "${TEST_ROOT}"
}
trap cleanup EXIT

fail() {
  printf '[assemble-selfcheck] FAIL: %s\n' "$*" >&2
  exit 1
}

assert_equal() {
  local expected="$1" actual="$2" message="$3"
  [ "${actual}" = "${expected}" ] \
    || fail "${message}: expected=${expected}, actual=${actual}"
}

write_fixture_file() {
  local path="$1" content="${2:-fixture}"
  mkdir -p "$(dirname "${path}")"
  printf '%s\n' "${content}" > "${path}"
}

configure_fixture_repo() {
  local work_dir="$1"
  git -C "${work_dir}" config user.name "Famo selfcheck"
  git -C "${work_dir}" config user.email "selfcheck@example.invalid"
}

create_ice_remote() {
  local work_dir="${TEST_ROOT}/ice-work"
  ICE_REMOTE="${TEST_ROOT}/ice.git"
  git init --quiet "${work_dir}"
  configure_fixture_repo "${work_dir}"

  write_fixture_file "${work_dir}/rime_ice.schema.yaml"
  write_fixture_file "${work_dir}/rime_ice.dict.yaml"
  write_fixture_file "${work_dir}/t9.schema.yaml"
  write_fixture_file "${work_dir}/melt_eng.schema.yaml"
  write_fixture_file "${work_dir}/double_pinyin_flypy.schema.yaml"
  write_fixture_file "${work_dir}/cn_dicts/base.dict.yaml"
  write_fixture_file "${work_dir}/.gitignore" "ignored-ice-residue.yaml"
  git -C "${work_dir}" add .
  git -C "${work_dir}" commit --quiet -m "pin target"
  ICE_PIN_SHA="$(git -C "${work_dir}" rev-parse HEAD)"
  git -C "${work_dir}" tag selfcheck-pin "${ICE_PIN_SHA}"
  git -C "${work_dir}" commit --quiet --allow-empty -m "default head"
  ICE_DEFAULT_SHA="$(git -C "${work_dir}" rev-parse HEAD)"
  git -C "${work_dir}" branch -M main

  git clone --quiet --bare "${work_dir}" "${ICE_REMOTE}"
  git --git-dir="${ICE_REMOTE}" symbolic-ref HEAD refs/heads/main
  git -C "${work_dir}" commit --quiet --allow-empty -m "wrong origin head"
  ICE_WRONG_REMOTE="${TEST_ROOT}/wrong-ice.git"
  git clone --quiet --bare "${work_dir}" "${ICE_WRONG_REMOTE}"
  git --git-dir="${ICE_WRONG_REMOTE}" symbolic-ref HEAD refs/heads/main
  git --git-dir="${ICE_WRONG_REMOTE}" update-ref refs/tags/selfcheck-pin "${ICE_DEFAULT_SHA}"
}

create_wubi_remote() {
  local work_dir="${TEST_ROOT}/wubi-work"
  WUBI_REMOTE="${TEST_ROOT}/wubi.git"
  git init --quiet "${work_dir}"
  configure_fixture_repo "${work_dir}"

  for file in \
    wubi86_jidian.schema.yaml \
    wubi86_jidian_pinyin.schema.yaml \
    wubi86_jidian_trad.schema.yaml \
    wubi86_jidian_trad_pinyin.schema.yaml \
    numbers.schema.yaml; do
    write_fixture_file "${work_dir}/${file}" $'switches:\nengine:\n  translators:\n    - table_translator'
  done
  write_fixture_file "${work_dir}/wubi86_jidian.dict.yaml"
  write_fixture_file "${work_dir}/pinyin_simp.dict.yaml"
  write_fixture_file "${work_dir}/lua/wubi86_jidian_date_translator.lua"
  write_fixture_file "${work_dir}/LICENSE"
  write_fixture_file "${work_dir}/.gitignore" "ignored-wubi-residue.schema.yaml"
  git -C "${work_dir}" add .
  git -C "${work_dir}" commit --quiet -m "pin target"
  WUBI_PIN_SHA="$(git -C "${work_dir}" rev-parse HEAD)"
  git -C "${work_dir}" tag selfcheck-pin "${WUBI_PIN_SHA}"
  git -C "${work_dir}" commit --quiet --allow-empty -m "default head"
  WUBI_DEFAULT_SHA="$(git -C "${work_dir}" rev-parse HEAD)"
  git -C "${work_dir}" branch -M master

  git clone --quiet --bare "${work_dir}" "${WUBI_REMOTE}"
  git --git-dir="${WUBI_REMOTE}" symbolic-ref HEAD refs/heads/master
  git -C "${work_dir}" commit --quiet --allow-empty -m "wrong origin head"
  WUBI_WRONG_REMOTE="${TEST_ROOT}/wrong-wubi.git"
  git clone --quiet --bare "${work_dir}" "${WUBI_WRONG_REMOTE}"
  git --git-dir="${WUBI_WRONG_REMOTE}" symbolic-ref HEAD refs/heads/master
  git --git-dir="${WUBI_WRONG_REMOTE}" update-ref refs/tags/selfcheck-pin "${WUBI_DEFAULT_SHA}"
}

make_case() {
  local name="$1" case_dir="${TEST_ROOT}/cases/$1"
  mkdir -p "${case_dir}"
  cp "${ASSEMBLE_SCRIPT}" "${case_dir}/assemble-payload.sh"
  cp -R "${CONFIG_DIR}/overlay" "${case_dir}/overlay"
  cp -R "${CONFIG_DIR}/opencc-standard" "${case_dir}/opencc-standard"
  printf '%s\n' "${case_dir}"
}

run_pinned() {
  local case_dir="$1" ice_ref="$2" wubi_ref="$3" log_file="$4"
  env ICE_REF="${ice_ref}" WUBI_REF="${wubi_ref}" \
    bash "${case_dir}/assemble-payload.sh" > "${log_file}" 2>&1
}

run_default() {
  local case_dir="$1" log_file="$2" git_config="${3:-${GIT_CONFIG_GLOBAL}}"
  env -u ICE_REF -u WUBI_REF GIT_CONFIG_GLOBAL="${git_config}" \
    bash "${case_dir}/assemble-payload.sh" > "${log_file}" 2>&1
}

repo_head() {
  local case_dir="$1" repo_dir="$2"
  git -C "${case_dir}/.cache/${repo_dir}" rev-parse HEAD
}

repo_origin() {
  local case_dir="$1" repo_dir="$2"
  git -C "${case_dir}/.cache/${repo_dir}" config --get remote.origin.url
}

seed_cache_residue() {
  local case_dir="$1"
  write_fixture_file "${case_dir}/.cache/rime-ice/untracked-ice-residue.yaml"
  write_fixture_file "${case_dir}/.cache/rime-ice/ignored-ice-residue.yaml"
  write_fixture_file \
    "${case_dir}/.cache/rime-wubi86-jidian/untracked-wubi-residue.schema.yaml"
  write_fixture_file \
    "${case_dir}/.cache/rime-wubi86-jidian/ignored-wubi-residue.schema.yaml"
  git -C "${case_dir}/.cache/rime-ice" check-ignore --quiet ignored-ice-residue.yaml \
    || fail "ICE ignored residue fixture is not ignored"
  git -C "${case_dir}/.cache/rime-wubi86-jidian" \
    check-ignore --quiet ignored-wubi-residue.schema.yaml \
    || fail "Wubi ignored residue fixture is not ignored"
}

assert_cache_residue_removed() {
  local case_dir="$1" message="$2" path
  for path in \
    ".cache/rime-ice/untracked-ice-residue.yaml" \
    ".cache/rime-ice/ignored-ice-residue.yaml" \
    ".cache/rime-wubi86-jidian/untracked-wubi-residue.schema.yaml" \
    ".cache/rime-wubi86-jidian/ignored-wubi-residue.schema.yaml" \
    "payload/untracked-ice-residue.yaml" \
    "payload/ignored-ice-residue.yaml" \
    "payload/untracked-wubi-residue.schema.yaml" \
    "payload/ignored-wubi-residue.schema.yaml"; do
    [ ! -e "${case_dir}/${path}" ] || fail "${message}: residue survived at ${path}"
  done
}

command -v git >/dev/null 2>&1 || fail "git not found"
[ -f "${ASSEMBLE_SCRIPT}" ] || fail "assemble script not found: ${ASSEMBLE_SCRIPT}"

export GIT_CONFIG_GLOBAL="${TEST_ROOT}/gitconfig"
export GIT_CONFIG_NOSYSTEM=1
export GIT_TERMINAL_PROMPT=0
export GIT_ALLOW_PROTOCOL=file

create_ice_remote
create_wubi_remote
git -C "${TEST_ROOT}" config --file "${GIT_CONFIG_GLOBAL}" \
  "url.file://${ICE_REMOTE}.insteadOf" "${ICE_REPO_URL}"
git -C "${TEST_ROOT}" config --file "${GIT_CONFIG_GLOBAL}" \
  "url.file://${WUBI_REMOTE}.insteadOf" "${WUBI_REPO_URL}"
OFFLINE_GIT_CONFIG="${TEST_ROOT}/offline-gitconfig"
git -C "${TEST_ROOT}" config --file "${OFFLINE_GIT_CONFIG}" \
  "url.file://${TEST_ROOT}/missing-canonical-ice.git.insteadOf" "${ICE_REPO_URL}"
git -C "${TEST_ROOT}" config --file "${OFFLINE_GIT_CONFIG}" \
  "url.file://${TEST_ROOT}/missing-canonical-wubi.git.insteadOf" "${WUBI_REPO_URL}"

first_sha_case="$(make_case first-sha)"
run_pinned "${first_sha_case}" "${ICE_PIN_SHA}" "${WUBI_PIN_SHA}" \
  "${TEST_ROOT}/first-sha.log"
assert_equal "${ICE_PIN_SHA}" "$(repo_head "${first_sha_case}" rime-ice)" \
  "first clone must honor ICE_REF SHA"
assert_equal "${WUBI_PIN_SHA}" "$(repo_head "${first_sha_case}" rime-wubi86-jidian)" \
  "first clone must honor WUBI_REF SHA"

cached_switch_case="$(make_case cached-switch)"
run_default "${cached_switch_case}" "${TEST_ROOT}/cached-default.log"
assert_equal "${ICE_DEFAULT_SHA}" "$(repo_head "${cached_switch_case}" rime-ice)" \
  "default clone must start ICE cache at the default HEAD"
assert_equal "${WUBI_DEFAULT_SHA}" "$(repo_head "${cached_switch_case}" rime-wubi86-jidian)" \
  "default clone must start Wubi cache at the default HEAD"
seed_cache_residue "${cached_switch_case}"
run_pinned "${cached_switch_case}" "${ICE_PIN_SHA}" "${WUBI_PIN_SHA}" \
  "${TEST_ROOT}/cached-pinned.log"
assert_equal "${ICE_PIN_SHA}" "$(repo_head "${cached_switch_case}" rime-ice)" \
  "cached ICE checkout must switch to the pinned SHA"
assert_equal "${WUBI_PIN_SHA}" "$(repo_head "${cached_switch_case}" rime-wubi86-jidian)" \
  "cached Wubi checkout must switch to the pinned SHA"
assert_cache_residue_removed "${cached_switch_case}" \
  "pinned cache checkout must clean managed repositories"

canonical_origin_case="$(make_case canonical-origin)"
run_default "${canonical_origin_case}" "${TEST_ROOT}/canonical-origin-default.log"
git -C "${canonical_origin_case}/.cache/rime-ice" remote set-url origin \
  "file://${ICE_WRONG_REMOTE}"
git -C "${canonical_origin_case}/.cache/rime-wubi86-jidian" remote set-url origin \
  "file://${WUBI_WRONG_REMOTE}"
run_pinned "${canonical_origin_case}" selfcheck-pin selfcheck-pin \
  "${TEST_ROOT}/canonical-origin-pinned.log"
assert_equal "${ICE_PIN_SHA}" "$(repo_head "${canonical_origin_case}" rime-ice)" \
  "pinned ICE ref must come from the canonical remote"
assert_equal "${WUBI_PIN_SHA}" "$(repo_head "${canonical_origin_case}" rime-wubi86-jidian)" \
  "pinned Wubi ref must come from the canonical remote"
assert_equal "${ICE_REPO_URL}" "$(repo_origin "${canonical_origin_case}" rime-ice)" \
  "pinned ICE checkout must restore the canonical origin"
assert_equal "${WUBI_REPO_URL}" "$(repo_origin "${canonical_origin_case}" rime-wubi86-jidian)" \
  "pinned Wubi checkout must restore the canonical origin"

canonical_default_case="$(make_case canonical-default)"
run_default "${canonical_default_case}" "${TEST_ROOT}/canonical-default-initial.log"
seed_cache_residue "${canonical_default_case}"
git -C "${canonical_default_case}/.cache/rime-ice" remote set-url origin \
  "file://${ICE_WRONG_REMOTE}"
git -C "${canonical_default_case}/.cache/rime-wubi86-jidian" remote set-url origin \
  "file://${WUBI_WRONG_REMOTE}"
run_default "${canonical_default_case}" "${TEST_ROOT}/canonical-default-updated.log"
assert_equal "${ICE_DEFAULT_SHA}" "$(repo_head "${canonical_default_case}" rime-ice)" \
  "unpinned ICE update must come from the canonical remote"
assert_equal "${WUBI_DEFAULT_SHA}" "$(repo_head "${canonical_default_case}" rime-wubi86-jidian)" \
  "unpinned Wubi update must come from the canonical remote"
assert_equal "${ICE_REPO_URL}" "$(repo_origin "${canonical_default_case}" rime-ice)" \
  "unpinned ICE update must restore the canonical origin"
assert_equal "${WUBI_REPO_URL}" "$(repo_origin "${canonical_default_case}" rime-wubi86-jidian)" \
  "unpinned Wubi update must restore the canonical origin"
assert_cache_residue_removed "${canonical_default_case}" \
  "unpinned online update must clean managed repositories"

ice_before_invalid="$(repo_head "${first_sha_case}" rime-ice)"
set +e
run_pinned "${first_sha_case}" refs/tags/missing-ice "${WUBI_PIN_SHA}" \
  "${TEST_ROOT}/invalid-ice.log"
invalid_ice_status=$?
set -e
[ "${invalid_ice_status}" -ne 0 ] || fail "invalid ICE_REF must fail closed"
assert_equal "${ice_before_invalid}" "$(repo_head "${first_sha_case}" rime-ice)" \
  "invalid ICE_REF must not replace the cached HEAD"
if grep -q '完成。' "${TEST_ROOT}/invalid-ice.log"; then
  fail "invalid ICE_REF must not report completion"
fi

wubi_before_invalid="$(repo_head "${first_sha_case}" rime-wubi86-jidian)"
set +e
run_pinned "${first_sha_case}" "${ICE_PIN_SHA}" refs/tags/missing-wubi \
  "${TEST_ROOT}/invalid-wubi.log"
invalid_wubi_status=$?
set -e
[ "${invalid_wubi_status}" -ne 0 ] || fail "invalid WUBI_REF must fail closed"
assert_equal "${wubi_before_invalid}" "$(repo_head "${first_sha_case}" rime-wubi86-jidian)" \
  "invalid WUBI_REF must not replace the cached HEAD"
if grep -q '完成。' "${TEST_ROOT}/invalid-wubi.log"; then
  fail "invalid WUBI_REF must not report completion"
fi

default_offline_case="$(make_case default-offline)"
run_default "${default_offline_case}" "${TEST_ROOT}/default-online.log"
default_ice_head="$(repo_head "${default_offline_case}" rime-ice)"
default_wubi_head="$(repo_head "${default_offline_case}" rime-wubi86-jidian)"
seed_cache_residue "${default_offline_case}"
git -C "${default_offline_case}/.cache/rime-ice" remote set-url origin \
  "file://${TEST_ROOT}/missing-default-ice.git"
git -C "${default_offline_case}/.cache/rime-wubi86-jidian" remote set-url origin \
  "file://${TEST_ROOT}/missing-default-wubi.git"
run_default "${default_offline_case}" "${TEST_ROOT}/default-offline.log" \
  "${OFFLINE_GIT_CONFIG}"
assert_equal "${default_ice_head}" "$(repo_head "${default_offline_case}" rime-ice)" \
  "unpinned offline mode must retain the cached ICE HEAD"
assert_equal "${default_wubi_head}" "$(repo_head "${default_offline_case}" rime-wubi86-jidian)" \
  "unpinned offline mode must retain the cached Wubi HEAD"
assert_equal "${ICE_REPO_URL}" "$(repo_origin "${default_offline_case}" rime-ice)" \
  "unpinned offline mode must retain the canonical ICE origin"
assert_equal "${WUBI_REPO_URL}" "$(repo_origin "${default_offline_case}" rime-wubi86-jidian)" \
  "unpinned offline mode must retain the canonical Wubi origin"
assert_cache_residue_removed "${default_offline_case}" \
  "unpinned offline mode must clean managed repositories"
offline_warning_count="$(grep -c '沿用已缓存' "${TEST_ROOT}/default-offline.log")"
assert_equal "2" "${offline_warning_count}" \
  "unpinned offline mode must report both cache fallbacks"
grep -q '完成。' "${TEST_ROOT}/default-offline.log" \
  || fail "unpinned offline mode must complete with valid caches"

printf '[assemble-selfcheck] PASS: first clone honors pinned SHAs\n'
printf '[assemble-selfcheck] PASS: cached repositories switch to pinned SHAs\n'
printf '[assemble-selfcheck] PASS: pinned refs restore canonical origins\n'
printf '[assemble-selfcheck] PASS: unpinned updates restore canonical origins\n'
printf '[assemble-selfcheck] PASS: invalid refs fail closed\n'
printf '[assemble-selfcheck] PASS: unpinned offline mode retains valid caches\n'
printf '[assemble-selfcheck] PASS: managed caches exclude untracked and ignored residue\n'
