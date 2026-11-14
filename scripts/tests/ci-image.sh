#!/usr/bin/env bash
# Host-only contract for image inputs and the container launch boundary.
set -euo pipefail
cd "$(dirname "$0")/../.."
unset RORTHOX_IN_IMAGE
root=$(pwd -P)
OUT=${RORTHOX_TEST_ROOT:-build/tests}/ci-image
mkdir -p "$OUT"
out=$(cd "$OUT" && pwd -P)
tmp=$(mktemp -d)
fallback=
work=
kvm_script=
unusable_script=
trap 'rm -rf "$tmp"; [ -z "$work" ] || rm -rf "$work"; [ -z "$fallback" ] || rm -rf "$fallback"; [ -z "$kvm_script" ] || rm -f "$kvm_script"; [ -z "$unusable_script" ] || rm -f "$unusable_script"' EXIT
fail() { echo "FAIL ci-image: $*" >&2; exit 1; }
for file in ci/Dockerfile ci/image-key.sh ci/run.sh ci/image.ref .devcontainer/devcontainer.json; do
    [ -f "$file" ] || fail "missing $file"
done
bash -n ci/image-key.sh ci/run.sh configure.sh
key=$(bash ci/image-key.sh)
[[ "$key" =~ ^[0-9a-f]{64}$ ]] || fail 'invalid image key'
mkdir -p "$tmp/copy/ci" "$tmp/context"
cp ci/Dockerfile ci/image-key.sh "$tmp/copy/ci/"
cp configure.sh "$tmp/copy/"
[ "$(bash "$tmp/copy/ci/image-key.sh")" = "$key" ] || fail 'key changes outside repository'
printf 'unrelated\n' > "$tmp/copy/unrelated"
[ "$(bash "$tmp/copy/ci/image-key.sh")" = "$key" ] || fail 'unrelated file changes key'
while IFS= read -r file; do
    printf '\n# changed\n' >> "$tmp/copy/$file"
    [ "$(bash "$tmp/copy/ci/image-key.sh")" != "$key" ] || fail "$file mutation did not change key"
    cp "$file" "$tmp/copy/$file"
done < <(bash ci/image-key.sh --inputs)
bash ci/image-key.sh --context "$tmp/context"
[ "$(find "$tmp/context" -type f | LC_ALL=C sort | sed "s|$tmp/context/||")" = "$(bash ci/image-key.sh --inputs)" ] || fail 'build context has unlisted files'
if bash ci/image-key.sh --context "$tmp/context" >/dev/null 2>&1; then fail 'nonempty context accepted'; fi
# The published digest is shared with the devcontainer.
ref=$(grep -v '^#' ci/image.ref | head -n1)
[[ "$ref" =~ ^ghcr\.io/oriapp/rorthox-build@sha256:[0-9a-f]{64}$ ]] || fail 'invalid reference'
python3 -B - <<'PY' || fail 'unsafe devcontainer keys'
import json
from pathlib import Path
item = json.loads(Path('.devcontainer/devcontainer.json').read_text())
def safe(value):
    return (set(value) == {'name', 'image', 'workspaceFolder', 'workspaceMount', 'remoteUser', 'containerEnv'}
            and set(value['containerEnv']) == {'RORTHOX_IN_IMAGE'})
assert safe(item)
assert {key: value for key, value in item.items() if key != 'image'} == {
    'name': 'RorthOX build',
    'workspaceFolder': '/work',
    'workspaceMount': 'source=${localWorkspaceFolder},target=/work,type=bind',
    'remoteUser': 'ubuntu',
    'containerEnv': {'RORTHOX_IN_IMAGE': '1'},
}
for key in ('runArgs', 'privileged', 'devices', 'extensions', 'customizations', 'mounts'):
    assert not safe({**item, key: True}), key
PY
[ "$(python3 -B -c 'import json; print(json.load(open(".devcontainer/devcontainer.json"))["image"])')" = "$ref" ] || fail 'devcontainer reference differs from image.ref'
[ "$(python3 -B -c 'import json; print(json.load(open(".devcontainer/devcontainer.json"))["containerEnv"]["RORTHOX_IN_IMAGE"])')" = 1 ] || fail 'devcontainer must run commands directly in image'
[ "$(grep -c '^FROM ' ci/Dockerfile)" -eq 2 ] || fail 'unexpected image stages'
if grep '^FROM ' ci/Dockerfile | grep -Ev '^FROM ubuntu:24\.04@sha256:[0-9a-f]{64}( AS [a-z]+)?$' >/dev/null; then
    fail 'unfixed base'
fi
grep -q 'snapshot.ubuntu.com/ubuntu/[0-9]\{8\}T[0-9]\{6\}Z' ci/Dockerfile || fail 'no apt snapshot'
[ "$(grep -c 'apt-get --error-on=any update && apt-get install -y' ci/Dockerfile)" -eq 2 ] || fail 'unverified apt refresh'
[ "$(grep -c 'bash git ca-certificates curl' ci/Dockerfile)" -eq 2 ] || fail 'CA absent from verified install'
! grep -Eq '(^|[[:space:]])(COPY[[:space:]]+\.|ADD[[:space:]]+https?://|latest)' ci/Dockerfile || fail 'unbounded Docker input'
grep -q 'org.opencontainers.image.licenses="GPL-2.0"' ci/Dockerfile || fail 'license label'
grep -q 'python3 python3-yaml' ci/Dockerfile || fail 'in-image workflow YAML parser missing'
# Fake docker records exact argv with NUL separators; no daemon is needed.
case "$out" in
    "$root"|"$root"/*) work="$out/work with spaces" ;;
    *) mkdir -p "$root/build"
       fallback=$(mktemp -d "$root/build/ci-image-work.XXXXXX")
       work="$fallback/work with spaces" ;;
esac
mkdir -p "$tmp/bin" "$work"
cat > "$tmp/bin/docker" <<'SH'
#!/usr/bin/env bash
printf '%s\0' "$@" > "$DOCKER_ARGS"
exit "${DOCKER_EXIT:-0}"
SH
chmod +x "$tmp/bin/docker"
digest=ghcr.io/oriapp/rorthox-build@sha256:1111111111111111111111111111111111111111111111111111111111111111
(
    cd "$work"
    export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/argv" DOCKER_EXIT=37
    unset RORTHOX_IN_IMAGE RORTHOX_TEST_ROOT RORTHOX_TEST_TIMEOUT RORTHOX_ACCEL_PROBE_TIMEOUT
    export RORTHOX_IMAGE_REF="$digest" GITHUB_ACTIONS=true RORTHOX_REQUIRE_ISO=1
    export GITHUB_TOKEN=secret ACTIONS_RUNTIME_TOKEN=secret GITHUB_ENV="$tmp/secret"
    export GITHUB_OUTPUT="$tmp/secret" GITHUB_STEP_SUMMARY="$tmp/secret"
    export RORTHOX_ACCEL=tcg
    if bash "$OLDPWD/ci/run.sh" /bin/printf '%s' 'a b'; then fail 'exit status lost'; else
        [ "$?" -eq 37 ] || fail 'wrong docker exit status'
    fi
)
mapfile -d '' -t actual < "$tmp/argv"
expected=(run --rm --init --user "$(id -u):$(id -g)" --network none
          --cap-drop ALL --security-opt no-new-privileges --pids-limit 512
          --memory 4g -v "$root:/work" -w "/work/${work#"$root"/}")
# Forced-TCG candidate jobs must not attach KVM even on a host where it is available.
expected+=(-e GITHUB_ACTIONS=true -e RORTHOX_ACCEL=tcg -e RORTHOX_REQUIRE_ISO=1
           "$digest" /bin/printf '%s' 'a b')
[ "${#expected[@]}" = "${#actual[@]}" ] || fail "docker argv length ${#actual[@]}, expected ${#expected[@]}"
for i in "${!expected[@]}"; do
    [ "${actual[i]}" = "${expected[i]}" ] || fail "docker argv[$i]: ${actual[i]}"
done
( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/default-argv" RORTHOX_ACCEL=tcg; unset RORTHOX_IN_IMAGE RORTHOX_IMAGE_REF
  bash ci/run.sh true ) 2> "$tmp/current-warning"
if grep -q 'warning: checkout image inputs differ from ci/image.ref' "$tmp/current-warning"; then
    fail 'current checkout warned about stale image inputs'
fi
mapfile -d '' -t default_args < "$tmp/default-argv"
# A regular file stands in for an unusable KVM device, regardless of the host.
touch "$tmp/unusable-kvm"
unusable_script=$(mktemp "$root/ci/.kvm-unusable.XXXXXX.sh")
sed "s|/dev/kvm|$tmp/unusable-kvm|g" ci/run.sh > "$unusable_script"
( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/forced-argv" RORTHOX_IMAGE_REF="$digest"
  export RORTHOX_ACCEL=kvm RORTHOX_ACCEL_PROBE_TIMEOUT=17
  unset RORTHOX_IN_IMAGE RORTHOX_TEST_ROOT
  bash "$unusable_script" true )
mapfile -d '' -t forced_args < "$tmp/forced-argv"
probe=0
for i in "${!forced_args[@]}"; do
    if [ "${forced_args[i]}" = -e ] && [ "${forced_args[i+1]:-}" = RORTHOX_ACCEL_PROBE_TIMEOUT=17 ]; then probe=1; fi
    [ "${forced_args[i]}" != --device ] || fail 'unusable KVM device forwarded'
done
[ "$probe" = 1 ] || fail 'KVM probe timeout not forwarded'
[[ " ${forced_args[*]} " == *' RORTHOX_ACCEL=kvm '* ]] || fail 'forced KVM not forwarded'
# Root ignores modes. Under the build user each case denies exactly one access.
if [ "$(id -u)" -ne 0 ]; then
    touch "$tmp/denied-kvm"
    for mode in 0400 0200; do
        chmod "$mode" "$tmp/denied-kvm"
        sed -e 's|\[ -c /dev/kvm \]|[ -c /dev/null ]|' \
            -e "s|\[ -r /dev/kvm \]|[ -r $tmp/denied-kvm ]|" \
            -e "s|\[ -w /dev/kvm \]|[ -w $tmp/denied-kvm ]|" ci/run.sh > "$unusable_script"
        grep -qF "[ -r $tmp/denied-kvm ]" "$unusable_script" || fail 'KVM read edit missed'
        grep -qF "[ -w $tmp/denied-kvm ]" "$unusable_script" || fail 'KVM write edit missed'
        grep -qF '[ -c /dev/null ]' "$unusable_script" || fail 'KVM device edit missed'
        ( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/denied-argv" RORTHOX_IMAGE_REF="$digest"
          export RORTHOX_ACCEL=auto
          unset RORTHOX_IN_IMAGE RORTHOX_TEST_ROOT
          bash "$unusable_script" true )
        mapfile -d '' -t denied_args < "$tmp/denied-argv"
        for i in "${!denied_args[@]}"; do
            [ "${denied_args[i]}" != --device ] || fail "inaccessible KVM device forwarded ($mode)"
        done
    done
fi
[ "${default_args[${#default_args[@]}-2]}" = "$ref" ] || fail 'default image differs from recorded digest'
# The flake hunt binds the container's cpuset, not only the Docker client.
allowed=$(awk '/^Cpus_allowed_list:/ {print $2}' /proc/self/status)
first=${allowed%%,*}
cpu=${first%%-*}
( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/cpuset-argv" RORTHOX_IMAGE_REF="$digest" RORTHOX_CPUSET="$cpu" RORTHOX_ACCEL=tcg
  bash ci/run.sh true )
mapfile -d '' -t cpuset_args < "$tmp/cpuset-argv"
cpuset=0
for i in "${!cpuset_args[@]}"; do
    if [ "${cpuset_args[i]}" = --cpuset-cpus ] && [ "${cpuset_args[i+1]:-}" = "$cpu" ]; then cpuset=1; fi
done
[ "$cpuset" = 1 ] || fail 'Docker container cpuset missing'
if ( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/cpuset-bad" RORTHOX_IMAGE_REF="$digest" RORTHOX_CPUSET=invalid
     bash ci/run.sh true ) >/dev/null 2>&1; then fail 'invalid cpuset accepted'; fi
# Changing an image input must warn for the locally pinned digest, without blocking it.
mkdir -p "$tmp/stale/ci"
cp ci/Dockerfile ci/image-key.sh ci/run.sh ci/image.ref "$tmp/stale/ci/"
cp configure.sh "$tmp/stale/"
printf '\n# local input change\n' >> "$tmp/stale/ci/Dockerfile"
( cd "$tmp/stale" || exit 1
  export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/stale-argv"
  unset RORTHOX_IN_IMAGE RORTHOX_IMAGE_REF
  bash ci/run.sh true ) 2> "$tmp/stale-warning" || fail 'stale image blocked'
grep -q 'warning: checkout image inputs differ from ci/image.ref' "$tmp/stale-warning" || fail 'missing stale image warning'
if ( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/argv" RORTHOX_IMAGE_REF=ghcr.io/oriapp/rorthox-build@sha256:0000000000000000000000000000000000000000000000000000000000000000
     bash ci/run.sh true ) >/dev/null 2>&1; then
    fail 'zero digest accepted'
fi
if ( cd "$tmp" && RORTHOX_IMAGE_REF="$digest" bash "$root/ci/run.sh" true ) >/dev/null 2>&1; then
    fail 'outside checkout accepted'
fi
if ( RORTHOX_TEST_ROOT="$tmp" RORTHOX_IMAGE_REF="$digest" bash ci/run.sh true ) >/dev/null 2>&1; then
    fail 'outside test root accepted'
fi
# The in-image path must bypass docker, even when the host fixture inherited RORTHOX_IN_IMAGE.
sed "s|/opt/rorthox-build-image|$tmp/in-image-marker|" ci/run.sh > "$tmp/in-image.sh"
touch "$tmp/in-image-marker"
[ "$(RORTHOX_IN_IMAGE=1 bash "$tmp/in-image.sh" printf '%s' in-image)" = in-image ] || fail 'in-image execution failed'
# /dev/null is a root-group character device only when its gid is mapped to zero.
kvm_script=$(mktemp "$root/ci/.kvm-fixture.XXXXXX.sh")
sed 's|/dev/kvm|/dev/null|g' ci/run.sh > "$kvm_script"
if [ "$(stat -c %g /dev/null)" = 0 ]; then
    ( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/kvm-argv" RORTHOX_IMAGE_REF="$digest" RORTHOX_ACCEL=auto
      unset RORTHOX_TEST_ROOT RORTHOX_IN_IMAGE
      bash "$kvm_script" true )
    mapfile -d '' -t kvm_args < "$tmp/kvm-argv"
    device=0
    for i in "${!kvm_args[@]}"; do
        [ "${kvm_args[i]}" != --group-add ] || fail 'root group forwarded'
        if [ "${kvm_args[i]}" = --device ] && [ "${kvm_args[i+1]:-}" = /dev/null ]; then device=1; fi
    done
    [ "$device" = 1 ] || fail 'character device not forwarded'
fi
# A nonzero device gid must reach docker even when the host primary gid differs.
cat > "$tmp/bin/stat" <<'SH'
#!/usr/bin/env bash
if [ "${1:-}" = -c ] && [ "${2:-}" = %g ] && [ "${3:-}" = /dev/null ]; then
    echo 4242
else
    exec /usr/bin/stat "$@"
fi
SH
chmod +x "$tmp/bin/stat"
( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/kvm-nonroot-argv" RORTHOX_IMAGE_REF="$digest" RORTHOX_ACCEL=auto
  unset RORTHOX_TEST_ROOT RORTHOX_IN_IMAGE
  bash "$kvm_script" true )
mapfile -d '' -t kvm_args < "$tmp/kvm-nonroot-argv"
device=0 group=0
for i in "${!kvm_args[@]}"; do
    if [ "${kvm_args[i]}" = --device ] && [ "${kvm_args[i+1]:-}" = /dev/null ]; then device=1; fi
    if [ "${kvm_args[i]}" = --group-add ] && [ "${kvm_args[i+1]:-}" = 4242 ]; then group=1; fi
done
[ "$device" = 1 ] && [ "$group" = 1 ] || fail 'nonzero KVM group was not forwarded'
# The host-usable device path must also pass the probe timeout and forced-KVM choice.
( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/kvm-forced-argv" RORTHOX_IMAGE_REF="$digest"
  export RORTHOX_ACCEL=kvm RORTHOX_ACCEL_PROBE_TIMEOUT=19
  unset RORTHOX_TEST_ROOT RORTHOX_IN_IMAGE
  bash "$kvm_script" true )
mapfile -d '' -t kvm_args < "$tmp/kvm-forced-argv"
[[ " ${kvm_args[*]} " == *' --device /dev/null '* ]] || fail 'usable character device not forwarded'
[[ " ${kvm_args[*]} " == *' RORTHOX_ACCEL_PROBE_TIMEOUT=19 '* ]] || fail 'KVM probe timeout missing from device run'
echo 'PASS ci-image: key, context, reference, Dockerfile, isolated argv and exit status'
