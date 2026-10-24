#!/usr/bin/env bash
# jlo_tool init and get-deps (vm/tools/deps.c), offline: the registry is a
# file:// directory of zip archives (JELLO_REGISTRY_URL).
set -euo pipefail

JLO_TOOL="${1:?usage: deps_fixtures.sh <jlo_tool>}"
# Absolute, as the checks run in the project.
JLO_TOOL="$(cd "$(dirname "${JLO_TOOL}")" && pwd)/$(basename "${JLO_TOOL}")"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
REG="${TMP}/registry"
export JELLO_REGISTRY_URL="file://${REG}"

failures=0
fail() {
  echo "deps_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

# check WHAT STATUS OUTPUT ARGS...: jlo_tool ARGS, in the project, exits
# STATUS and prints OUTPUT.
check() {
  local what="$1" want_status="$2" want="$3" out status
  shift 3
  out="$(cd "${TMP}/app" && "${JLO_TOOL}" "$@" 2>&1)" && status=0 || status=$?
  if [[ "${status}" != "${want_status}" || "${out}" != "${want}" ]]; then
    fail "${what}: expected status ${want_status} [${want}], got ${status} [${out}]"
  fi
}

# package NAME VERSION DEPS [TOP]: the archive of package NAME at VERSION,
# depending on DEPS (a JSON object), under the one directory TOP, as GitHub's
# archives are (NAME-VERSION by default).
package() {
  local name="$1" version="$2" deps="$3" top="${4-$1-$2}"
  mkdir -p "${REG}/packages/${name}/${version}"
  python3 - "${REG}/packages/${name}/${version}/artifact" "${top:+${top}/}" "${name}" "${version}" "${deps}" <<'EOF'
import sys, zipfile
path, top, name, version, deps = sys.argv[1:]
# A fixed date and mode make a package's bytes, and so its SHA-256, reproducible.
def entry(file):
    info = zipfile.ZipInfo(top + file, date_time=(1980, 1, 1, 0, 0, 0))
    info.external_attr = 0o600 << 16
    return info
with zipfile.ZipFile(path, 'w') as z:
    z.writestr(entry('jello.json'), '{"name": "%s", "version": "%s", "dependencies": %s}\n' % (name, version, deps))
    z.writestr(entry('src/' + name + '/Main.jello'), 'package %s;\n' % name)
EOF
}

sha() { python3 -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$1"; }

# lock NAME VERSION...: the jello.lock pinning these registry packages.
lock() {
  local first=1
  printf '{\n  "version": 2,\n  "packages": {'
  while [[ $# -gt 0 ]]; do
    [[ ${first} == 1 ]] && printf '\n' || printf ',\n'
    first=0
    printf '    "%s": {\n      "version": "%s",\n      "source": {\n        "url": "%s",\n        "sha256": "%s"\n      }\n    }' \
      "$1" "$2" "${JELLO_REGISTRY_URL}/packages/$1/$2/artifact" "$(sha "${REG}/packages/$1/$2/artifact")"
    shift 2
  done
  [[ ${first} == 1 ]] && printf '}\n}\n' || printf '\n  }\n}\n'
}

manifest() { printf '{\n  "name": "app",\n  "version": "0.1.0",\n  "dependencies": %s\n}\n' "$1" > "${TMP}/app/jello.json"; }

# init makes a project that names itself after its directory, and only once.
out="$("${JLO_TOOL}" init "${TMP}/app" 2>&1)" || fail "init: ${out}"
[[ "$(cat "${TMP}/app/jello.json")" == $'{\n  "name": "app",\n  "version": "0.1.0",\n  "dependencies": {}\n}' ]] ||
  fail "init wrote jello.json: $(cat "${TMP}/app/jello.json")"
[[ -f "${TMP}/app/src/Main.jello" && -f "${TMP}/app/.gitignore" ]] || fail "init wrote no src/Main.jello or .gitignore"
check "init twice" 1 "jlo_tool: . is already a project: it has jello.json" init
out="$("${JLO_TOOL}" init "${TMP}/bad name" 2>&1)" && fail "init took a bad name" || true
[[ "${out}" == *'"bad name" cannot name a package'* ]] || fail "init with a bad name said: ${out}"
"${JLO_TOOL}" init "${TMP}/bad name" --name good > /dev/null 2>&1 || fail "init --name"
check "nothing to get" 0 "dependencies up to date" get-deps
[[ ! -e "${TMP}/app/jello.lock" && ! -e "${TMP}/app/packages" ]] || fail "get-deps without dependencies wrote a lock or packages/"

# A dependency and its own come flat under packages/, pinned in the lock.
package alpha 1.0.0 '{"beta": "2.0.0"}'
package beta 2.0.0 '{}'
manifest '{"alpha": "1.0.0"}'
check "get" 0 $'fetched alpha 1.0.0\nfetched beta 2.0.0' get-deps
[[ "$(cat "${TMP}/app/jello.lock")" == "$(lock alpha 1.0.0 beta 2.0.0)" ]] || fail "the lock: $(cat "${TMP}/app/jello.lock")"
[[ -f "${TMP}/app/packages/alpha/src/alpha/Main.jello" && -f "${TMP}/app/packages/beta/jello.json" ]] ||
  fail "packages/ holds $(cd "${TMP}/app" && find packages | sort)"
[[ -z "$(cd "${TMP}/app/packages" && ls -A | grep '^\.')" ]] || fail "get-deps left $(ls -A "${TMP}/app/packages")"
cp "${TMP}/app/jello.lock" "${TMP}/lock.before"
check "again" 0 "dependencies up to date" get-deps
cmp -s "${TMP}/app/jello.lock" "${TMP}/lock.before" || fail "an idle get-deps changed the lock"

# A missing package comes back as the lock pins it; an archive that is not
# what the lock pins changes nothing.
rm -rf "${TMP}/app/packages/beta"
check "refetch" 0 "fetched beta 2.0.0" get-deps
rm -rf "${TMP}/app/packages/beta"
package beta 2.0.0 '{"alpha": "1.0.0"}'
url="${JELLO_REGISTRY_URL}/packages/beta/2.0.0/artifact"
check "tampered" 1 "jlo_tool: ${url} has SHA-256 $(sha "${REG}/packages/beta/2.0.0/artifact"), but jello.lock pins $(sed -n 's/.*"sha256": "\(.*\)"/\1/p' "${TMP}/lock.before" | tail -1) for beta" get-deps
cmp -s "${TMP}/app/jello.lock" "${TMP}/lock.before" || fail "a tampered archive changed the lock"
[[ ! -e "${TMP}/app/packages/beta" ]] || fail "a tampered archive was unpacked"
package beta 2.0.0 '{}'
check "restored" 0 "fetched beta 2.0.0" get-deps

# Another version is fetched and pinned; what nothing asks for goes.
package alpha 1.1.0 '{}'
manifest '{"alpha": "1.1.0"}'
check "upgrade" 0 $'fetched alpha 1.1.0\nremoved beta' get-deps
[[ "$(cat "${TMP}/app/jello.lock")" == "$(lock alpha 1.1.0)" && ! -e "${TMP}/app/packages/beta" ]] || fail "after the upgrade: $(cat "${TMP}/app/jello.lock")"

# A path dependency is linked in place, and not pinned.
"${JLO_TOOL}" init "${TMP}/near" > /dev/null 2>&1
manifest '{"alpha": "1.1.0", "near": {"path": "../near"}}'
check "path" 0 "linked near to ../near" get-deps
[[ "$(readlink "${TMP}/app/packages/near")" == "../../near" && "$(cat "${TMP}/app/jello.lock")" == "$(lock alpha 1.1.0)" ]] ||
  fail "path: $(readlink "${TMP}/app/packages/near") $(cat "${TMP}/app/jello.lock")"
check "path again" 0 "dependencies up to date" get-deps
manifest '{"alpha": "1.1.0"}'
check "path dropped" 0 "removed near" get-deps
[[ ! -e "${TMP}/app/packages/near" && -d "${TMP}/near" ]] || fail "dropping a path dependency"

# The project's dev_dependencies are fetched; a package's are not (epsilon has
# no archive). The JSON after each `}` extends the manifest.
package kappa 1.0.0 '{}, "dev_dependencies": {"epsilon": "1.0.0"}'
manifest '{"alpha": "1.1.0"}, "dev_dependencies": {"kappa": "1.0.0"}'
check "dev" 0 "fetched kappa 1.0.0" get-deps
[[ "$(cat "${TMP}/app/jello.lock")" == "$(lock alpha 1.1.0 kappa 1.0.0)" ]] || fail "dev: $(cat "${TMP}/app/jello.lock")"
manifest '{"alpha": "1.1.0"}, "dev_dependencies": 3'
check "bad dev" 1 "jlo_tool: ./jello.json: \"dev_dependencies\" must be an object" get-deps
manifest '{"alpha": "1.1.0"}'
check "dev dropped" 0 "removed kappa" get-deps

# What get-deps refuses.
package gamma 1.0.0 '{"alpha": "1.0.0"}'
manifest '{"alpha": "1.1.0", "gamma": "1.0.0"}'
check "two versions" 1 "fetched gamma 1.0.0
jlo_tool: jello.json asks for alpha from ${JELLO_REGISTRY_URL}/packages/alpha/1.1.0/artifact, and gamma from ${JELLO_REGISTRY_URL}/packages/alpha/1.0.0/artifact; there is one copy of each package" get-deps
package delta 1.0.0 '{}' ""
package wrong 1.0.0 '{}'
mv "${REG}/packages/wrong/1.0.0/artifact" "${REG}/packages/delta/1.0.0/artifact"
manifest '{"delta": "1.0.0"}'
check "wrong name" 1 "jlo_tool: ${JELLO_REGISTRY_URL}/packages/delta/1.0.0/artifact holds package \"wrong\", not \"delta\"" get-deps
package delta 2.0.0 '{}'
mv "${REG}/packages/delta/2.0.0/artifact" "${REG}/packages/delta/1.0.0/artifact"
check "wrong version" 1 "jlo_tool: ${JELLO_REGISTRY_URL}/packages/delta/1.0.0/artifact holds delta version 2.0.0, not 1.0.0" get-deps
manifest '{"epsilon": "1.0.0"}'
# curl says why in its own words, which vary by version.
out="$(cd "${TMP}/app" && "${JLO_TOOL}" get-deps 2>&1)" && fail "a missing archive was fetched" || true
[[ "$(grep -v '^curl:' <<<"${out}")" == "jlo_tool: cannot fetch ${JELLO_REGISTRY_URL}/packages/epsilon/1.0.0/artifact for epsilon" ]] || fail "not there: ${out}"
manifest '{"eta": "../1.0.0"}'
check "bad version" 1 "jlo_tool: ./jello.json: dependency \"eta\" takes a version, {\"github\": \"owner/repo\", \"tag\" or \"commit\"} or {\"path\"}" get-deps
# Only the project names a path: a fetched package's would reach anywhere.
package theta 1.0.0 '{"near": {"path": "../../near"}}'
manifest '{"theta": "1.0.0"}'
check "path from a package" 1 "fetched theta 1.0.0
jlo_tool: ./packages/theta/jello.json: dependency \"near\" takes a version, {\"github\": \"owner/repo\", \"tag\" or \"commit\"}; only the project's own jello.json names a path" get-deps
# An archive holding a link, as unzip recreates it.
mkdir -p "${REG}/packages/iota/1.0.0"
python3 - "${REG}/packages/iota/1.0.0/artifact" <<'EOF2'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr('iota-1.0.0/jello.json', '{"name": "iota", "version": "1.0.0"}\n')
    link = zipfile.ZipInfo('iota-1.0.0/escape')
    link.create_system = 3
    link.external_attr = 0o120777 << 16
    z.writestr(link, '/etc')
EOF2
manifest '{"iota": "1.0.0"}'
check "a link" 1 "jlo_tool: ${JELLO_REGISTRY_URL}/packages/iota/1.0.0/artifact holds a symbolic link; packages may not" get-deps
[[ ! -e "${TMP}/app/packages/iota" ]] || fail "a package with a link was unpacked"
manifest '{"a+b": "1.0.0"}'
check "bad name" 1 "jlo_tool: ./jello.json: bad dependency name \"a+b\"" get-deps
manifest '{"zeta": {"github": "owner/zeta"}}'
check "bad spec" 1 "jlo_tool: ./jello.json: dependency \"zeta\" takes a version, {\"github\": \"owner/repo\", \"tag\" or \"commit\"} or {\"path\"}" get-deps
printf '{"name": "app", "dependencies": {' > "${TMP}/app/jello.json"
check "bad JSON" 1 "jlo_tool: ./jello.json: invalid JSON at byte 33: expected a string" get-deps
rm "${TMP}/app/jello.json"
check "no manifest" 1 "jlo_tool: no jello.json in . (jlo_tool init makes one)" get-deps

if [[ ${failures} -gt 0 ]]; then
  echo "deps_fixtures: ${failures} failure(s)" >&2
  exit 1
fi
echo "deps_fixtures: passed"
