#!/usr/bin/env bash
# Smoke test: is the workbench operational?
#
# Three layers, in order of what they can prove:
#   1 readiness    doctor -- tools present, right versions, gate wired
#   2 unit         pure logic, no hardware
#   3 integration  real PlatformIO: template builds, upload hook fires
#
# Layer 4 (real silicon) needs a board. See "first contact" in README.
#
# Exit 0 = all green.  1 = degraded (warnings only).  2 = something failed.

set -uo pipefail
cd "$(dirname "$0")"
fails=0; warns=0

hdr() { printf '\n\033[1m%s\033[0m\n' "$1"; }
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fails=$((fails+1)); }
wrn() { printf '  \033[33mwarn\033[0m %s\n' "$1"; warns=$((warns+1)); }

hdr "1. readiness"
python3 tools/doctor.py > /tmp/esp32_doctor.$$ 2>&1
case $? in
  0) ok "doctor: operational" ;;
  1) wrn "doctor: degraded (see: python3 tools/doctor.py)"
     grep -E "^  WARN" /tmp/esp32_doctor.$$ | sed 's/^/    /' ;;
  *) bad "doctor: BROKEN"
     grep -E "^  FAIL" /tmp/esp32_doctor.$$ | sed 's/^/    /' ;;
esac
rm -f /tmp/esp32_doctor.$$

hdr "2. unit tests"
total=0
for t in tools/test_*.py; do
  out=$(python3 "$t" 2>&1); rc=$?
  n=$(printf '%s' "$out" | grep -c "  PASS")
  total=$((total+n))
  if [ $rc -eq 0 ]; then ok "$(basename "$t") — $n passing"
  else bad "$(basename "$t")"; printf '%s\n' "$out" | grep "  FAIL" | sed 's/^/    /'; fi
done
ok "$total unit assertions total"

hdr "3. profile schema"
if python3 tools/validate_profiles.py --quiet; then ok "all profiles valid"
else bad "profile validation"; fi

hdr "4. integration (real ESP-IDF)"
IDF_VER=$(cat .idf-version 2>/dev/null || echo v6.0.3)
IDF_ROOT="$HOME/esp/esp-idf-${IDF_VER}"
IDF_TOOLS="$HOME/.espressif-${IDF_VER#v}"
if [ ! -f "$IDF_ROOT/export.sh" ]; then
  wrn "ESP-IDF $IDF_VER not installed — build and gate checks skipped"
else
  work=".smoke_idf"; rm -rf "$work"; mkdir -p "$work"
  cp -r templates/idf-base/. "$work"/
  (
    export IDF_TOOLS_PATH="$IDF_TOOLS"
    . "$IDF_ROOT/export.sh" >/dev/null 2>&1
    cd "$work"
    idf.py set-target esp32c6 >/dev/null 2>&1
    idf.py build > /tmp/esp32_idf_build.$$ 2>&1
  ) && ok "idf template builds (esp32c6)"     || { bad "idf template build"; tail -12 /tmp/esp32_idf_build.$$ | sed 's/^/    /'; }

  # The gate must actually fire. The bypass path prints before any port is
  # needed, so this proves the hook without a board attached — same trick as
  # the PlatformIO layer.
  hook=$(
    export IDF_TOOLS_PATH="$IDF_TOOLS"
    . "$IDF_ROOT/export.sh" >/dev/null 2>&1
    cd "$work" && ESP32_NO_GATE=1 idf.py flash 2>&1 || true
  )
  if printf '%s' "$hook" | grep -q "BACKUP GATE BYPASSED"; then
    ok "pre-flash gate fires on 'idf.py flash'"
  else
    bad "pre-flash gate did NOT fire — idf.py flash is unguarded"
    printf '%s\n' "$hook" | grep -iE "error|traceback|warning.*extension" | head -3 | sed 's/^/    /'
  fi

  # And it must NOT fire on a non-write action.
  quiet=$(
    export IDF_TOOLS_PATH="$IDF_TOOLS"
    . "$IDF_ROOT/export.sh" >/dev/null 2>&1
    cd "$work" && idf.py --dry-run flash 2>&1 || true
  )
  if printf '%s' "$quiet" | grep -q "UPLOAD BLOCKED"; then
    bad "gate blocks --dry-run flash, which writes nothing"
  else
    ok "gate correctly ignores --dry-run flash"
  fi
  rm -rf "$work" /tmp/esp32_idf_build.$$
fi

hdr "5. integration (PlatformIO — optional Arduino path)"
if ! command -v pio > /dev/null 2>&1; then
  ok "pio not installed (optional Arduino path)"
else
  work=".smoke_build"; rm -rf "$work"; mkdir -p "$work"; cp -r templates/pio-base/. "$work"/
  if (cd "$work" && pio run -e esp32dev > /tmp/esp32_build.$$ 2>&1); then
    ok "template compiles (esp32dev)"
  else
    bad "template build"; tail -12 /tmp/esp32_build.$$ | sed 's/^/    /'
  fi

  # The upload hook must actually fire. The bypass path proves it, because it
  # prints before any port is needed -- so this works with no board attached.
  hook=$(cd "$work" && ESP32_NO_GATE=1 pio run -e esp32dev -t upload 2>&1 || true)
  if printf '%s' "$hook" | grep -q "BACKUP GATE BYPASSED"; then
    ok "pre-upload gate fires on 'pio run -t upload'"
  else
    bad "pre-upload gate did NOT fire — pio upload is unguarded"
    printf '%s\n' "$hook" | grep -iE "error|traceback|nameerror" | head -3 | sed 's/^/    /'
  fi
  if printf '%s' "$hook" | grep -qiE "NameError|Traceback"; then
    bad "gate hook raised an exception"
  fi
  rm -rf "$work" /tmp/esp32_build.$$
fi

hdr "result"
if [ $fails -gt 0 ]; then printf '  \033[31m%d failure(s), %d warning(s)\033[0m\n\n' $fails $warns; exit 2; fi
if [ $warns -gt 0 ]; then printf '  \033[33m%d warning(s), no failures — core paths operational\033[0m\n\n' $warns; exit 1; fi
printf '  \033[32mall green\033[0m\n\n'; exit 0
