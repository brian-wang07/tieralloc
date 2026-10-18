#!/usr/bin/env bash
# Builds the pinned baseline allocators (jemalloc, mimalloc, Google tcmalloc) as
# shared libraries for LD_PRELOAD, checks each one, and writes
# <prefix>/manifest.json for the bench runner. glibc is the system's and is
# only recorded.
#
# Huge pages are runtime settings, not build options: the manifest lists each
# allocator's hugepage environment and the runner applies it per
# configuration. The matrix itself runs with system THP set to "always".
#
# Usage: scripts/build-allocators.sh [--prefix=DIR] [--jobs=N]
#          [--only=jemalloc,mimalloc,tcmalloc] [--force] [--selftest=PATH]
#
# An allocator whose recorded pin matches is skipped unless --force is given.

set -euo pipefail

# Pins. Change a version only together with its hash or commit.
JEMALLOC_VERSION=5.4.0
JEMALLOC_SHA256=200776fac271093e7c2f21edd6d62657ecd2be578d9328633f2a86bfa6ef4f1d
JEMALLOC_URL=https://github.com/jemalloc/jemalloc/releases/download/${JEMALLOC_VERSION}/jemalloc-${JEMALLOC_VERSION}.tar.bz2
MIMALLOC_TAG=v3.5.3
MIMALLOC_COMMIT=d4881d338125e1cb7c47ba4cfb398d6f7c0c8d45
MIMALLOC_URL=https://github.com/microsoft/mimalloc.git
# Must match the git_override in scripts/tcmalloc/MODULE.bazel.
TCMALLOC_COMMIT=8ab5266c53294d06c0884dd45ef7529b1c08ab67
BAZELISK_VERSION=v1.29.0
BAZELISK_SHA256_AMD64=5a408715e932c0250d28bd84555f12edbf70117de42f9181691c736eacc4a992
BAZELISK_SHA256_ARM64=e20e8b0f4f240091b7a55bf17b9398bd4f40ee70ae0208dff95dd4c445fb4010

# Hugepage settings per allocator, recorded in the manifest. tcmalloc has none:
# it never calls MADV_HUGEPAGE and relies on system THP=always.
GLIBC_HUGEPAGE_ENV='GLIBC_TUNABLES=glibc.malloc.hugetlb=1'
# abort_conf makes an option 5.4.0 no longer knows abort instead of warn.
JEMALLOC_HUGEPAGE_ENV='MALLOC_CONF=thp:always,metadata_thp:always,abort_conf:true'
MIMALLOC_HUGEPAGE_ENV='MIMALLOC_ALLOW_LARGE_OS_PAGES=1'

# Exported by every working malloc replacement; checked with nm -D.
REQUIRED_SYMBOLS=(malloc free calloc realloc posix_memalign aligned_alloc
  malloc_usable_size _Znwm _ZdlPv _ZdlPvm _ZnwmSt11align_val_t _ZdlPvmSt11align_val_t)

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PREFIX=$ROOT/build-deps
JOBS=$(nproc)
ONLY=jemalloc,mimalloc,tcmalloc
FORCE=0
SELFTEST=$ROOT/build/bin/harness-selftest

for arg in "$@"; do
  case $arg in
  --prefix=*) PREFIX=$(realpath -m "${arg#*=}") ;;
  --jobs=*) JOBS=${arg#*=} ;;
  --only=*) ONLY=${arg#*=} ;;
  --force) FORCE=1 ;;
  --selftest=*) SELFTEST=$(realpath -m "${arg#*=}") ;;
  -h | --help)
    sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  *)
    echo "unknown argument: $arg (see --help)" >&2
    exit 2
    ;;
  esac
done

log() { printf '==> %s\n' "$*"; }
die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

wanted() { [[ ",$ONLY," == *",$1,"* ]]; }

for tool in curl git cmake make cc c++ nm sha256sum python3 taskset tar; do
  command -v "$tool" >/dev/null || die "missing required tool: $tool"
done

mkdir -p "$PREFIX"/{downloads,src,build,lib,bin,logs}

# A field recorded in a built allocator's build-info.json, or empty.
recorded_field() {
  local info=$PREFIX/lib/$1/build-info.json
  [[ -f $info ]] || return 0
  python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2],""))' \
    "$info" "$2"
}

# The compiler is part of the key: after a toolchain upgrade every allocator
# must be rebuilt, or the baselines silently keep the old compiler's code.
up_to_date() {
  local name=$1 pin=$2 lib=$3
  [[ $FORCE == 0 && -f $lib && "$(recorded_field "$name" pin)" == "$pin" &&
    "$(recorded_field "$name" compiler)" == "$(compiler_id)" ]]
}

check_sha256() {
  local file=$1 expected=$2
  local actual
  actual=$(sha256sum "$file" | cut -d' ' -f1)
  [[ $actual == "$expected" ]] || die "sha256 mismatch for $file: got $actual, expected $expected"
}

check_symbols() {
  local lib=$1 missing=()
  local exported
  exported=$(nm -D --defined-only "$lib" | awk '{print $NF}')
  for symbol in "${REQUIRED_SYMBOLS[@]}"; do
    grep -qx -- "$symbol" <<<"$exported" || missing+=("$symbol")
  done
  ((${#missing[@]} == 0)) || die "$lib does not export: ${missing[*]}"
}

# Runs the harness selftest with the library preloaded, plain and with its
# hugepage environment, and requires it to report the expected allocator.
# Timing checks are skipped (they measure the machine, not the allocator) and
# counter checks skip themselves if counters are unavailable.
smoke_test() {
  local name=$1 lib=$2 hugepage_env=$3
  local cpu=$(($(nproc) - 1))
  local logfile=$PREFIX/logs/$name-selftest.log
  : >"$logfile"
  local -a settings=("")
  if [[ -n $hugepage_env ]]; then settings+=("$hugepage_env"); fi
  for env_setting in "${settings[@]}"; do
    local -a env_args=()
    if [[ -n $env_setting ]]; then env_args=("$env_setting"); fi
    {
      echo "### LD_PRELOAD=$lib ${env_setting:-(no hugepage env)}"
    } >>"$logfile"
    if ! env "${env_args[@]}" LD_PRELOAD="$lib" taskset -c "$cpu" \
      "$SELFTEST" --expect-allocator="$name" --no-timing >>"$logfile" 2>&1; then
      die "selftest failed with $name preloaded${env_setting:+ and $env_setting}; see $logfile"
    fi
    # ld.so only warns about a preload it cannot load, so check it was used.
    if grep -q "cannot be preloaded" "$logfile"; then
      die "ld.so ignored $lib; see $logfile"
    fi
  done
}

# Writes <prefix>/lib/<name>/build-info.json from key=value arguments.
write_info() {
  local name=$1
  shift
  python3 -I - "$PREFIX/lib/$name/build-info.json" "$@" <<'EOF'
import json, sys
path, pairs = sys.argv[1], sys.argv[2:]
info = dict(pair.split("=", 1) for pair in pairs)
with open(path, "w") as f:
    json.dump(info, f, indent=2)
    f.write("\n")
EOF
}

compiler_id() { cc --version | head -1; }

ensure_selftest() {
  if [[ ! -x $SELFTEST ]]; then
    log "building harness-selftest"
    cmake -S "$ROOT" -B "$ROOT/build" >/dev/null
    cmake --build "$ROOT/build" --target harness-selftest -j "$JOBS" >/dev/null
  fi
  [[ -x $SELFTEST ]] || die "no selftest binary at $SELFTEST"
}

build_jemalloc() {
  local pin="jemalloc-$JEMALLOC_VERSION sha256:$JEMALLOC_SHA256"
  local install=$PREFIX/lib/jemalloc
  local lib=$install/lib/libjemalloc.so.2
  if up_to_date jemalloc "$pin" "$lib"; then
    log "jemalloc $JEMALLOC_VERSION up to date"
    return
  fi
  log "jemalloc $JEMALLOC_VERSION"
  local tarball=$PREFIX/downloads/jemalloc-$JEMALLOC_VERSION.tar.bz2
  [[ -f $tarball ]] || curl -fsSL -o "$tarball" "$JEMALLOC_URL"
  check_sha256 "$tarball" "$JEMALLOC_SHA256"
  local src=$PREFIX/src/jemalloc-$JEMALLOC_VERSION
  rm -rf "$src" "$install"
  tar -xjf "$tarball" -C "$PREFIX/src"
  # No symbol prefix, so it replaces malloc; C++ support stays on, as shipped.
  local configure_flags="--prefix=$install"
  (
    cd "$src"
    ./configure $configure_flags >"$PREFIX/logs/jemalloc-configure.log" 2>&1
    make -j "$JOBS" >"$PREFIX/logs/jemalloc-build.log" 2>&1
    make install_lib_shared install_include >>"$PREFIX/logs/jemalloc-build.log" 2>&1
  ) || die "jemalloc build failed; see $PREFIX/logs/jemalloc-*.log"
  check_symbols "$lib"
  smoke_test jemalloc "$lib" "$JEMALLOC_HUGEPAGE_ENV"
  write_info jemalloc name=jemalloc version="$JEMALLOC_VERSION" pin="$pin" \
    source="$JEMALLOC_URL" library="$lib" buildFlags="$configure_flags" \
    compiler="$(compiler_id)" hugepageEnv="$JEMALLOC_HUGEPAGE_ENV"
}

build_mimalloc() {
  local pin="mimalloc-$MIMALLOC_TAG commit:$MIMALLOC_COMMIT"
  local install=$PREFIX/lib/mimalloc
  local lib=$install/libmimalloc.so
  if up_to_date mimalloc "$pin" "$lib"; then
    log "mimalloc $MIMALLOC_TAG up to date"
    return
  fi
  log "mimalloc $MIMALLOC_TAG"
  local src=$PREFIX/src/mimalloc-$MIMALLOC_TAG
  local build=$PREFIX/build/mimalloc
  rm -rf "$src" "$build" "$install"
  git clone --quiet --depth 1 --branch "$MIMALLOC_TAG" "$MIMALLOC_URL" "$src" 2>/dev/null
  local head
  head=$(git -C "$src" rev-parse HEAD)
  [[ $head == "$MIMALLOC_COMMIT" ]] || die "mimalloc $MIMALLOC_TAG is $head, expected $MIMALLOC_COMMIT"
  local cmake_flags="-DCMAKE_BUILD_TYPE=Release -DMI_OVERRIDE=ON -DMI_BUILD_TESTS=OFF -DMI_BUILD_STATIC=OFF -DMI_BUILD_OBJECT=OFF"
  (
    cmake -S "$src" -B "$build" $cmake_flags >"$PREFIX/logs/mimalloc-configure.log" 2>&1
    cmake --build "$build" -j "$JOBS" >"$PREFIX/logs/mimalloc-build.log" 2>&1
  ) || die "mimalloc build failed; see $PREFIX/logs/mimalloc-*.log"
  # The build names the library after its version; install it under a stable
  # name, resolving the soname symlinks.
  local built
  built=$(find "$build" -maxdepth 1 -name 'libmimalloc.so.*' -type f | head -1)
  [[ -n $built ]] || die "mimalloc build produced no libmimalloc.so.*"
  mkdir -p "$install"
  cp "$built" "$lib"
  cp -r "$src/include" "$install/include"
  check_symbols "$lib"
  smoke_test mimalloc "$lib" "$MIMALLOC_HUGEPAGE_ENV"
  # The environment name is not validated by mimalloc, so confirm it was read.
  # /usr/bin/true, not the builtin, so the preload actually happens.
  env MIMALLOC_VERBOSE=1 MIMALLOC_ALLOW_LARGE_OS_PAGES=1 LD_PRELOAD="$lib" /usr/bin/true 2>&1 |
    grep -Eq "allow_large_os_pages'?:? *1" ||
    die "mimalloc did not pick up MIMALLOC_ALLOW_LARGE_OS_PAGES=1"
  write_info mimalloc name=mimalloc version="${MIMALLOC_TAG#v}" pin="$pin" \
    source="$MIMALLOC_URL@$MIMALLOC_TAG" library="$lib" buildFlags="$cmake_flags" \
    compiler="$(compiler_id)" hugepageEnv="$MIMALLOC_HUGEPAGE_ENV"
}

ensure_bazelisk() {
  local arch sha
  case $(uname -m) in
  x86_64) arch=amd64 sha=$BAZELISK_SHA256_AMD64 ;;
  aarch64) arch=arm64 sha=$BAZELISK_SHA256_ARM64 ;;
  *) die "no pinned bazelisk for $(uname -m)" ;;
  esac
  BAZELISK=$PREFIX/bin/bazelisk-$BAZELISK_VERSION
  if [[ ! -x $BAZELISK ]]; then
    curl -fsSL -o "$BAZELISK.tmp" \
      "https://github.com/bazelbuild/bazelisk/releases/download/$BAZELISK_VERSION/bazelisk-linux-$arch"
    check_sha256 "$BAZELISK.tmp" "$sha"
    chmod +x "$BAZELISK.tmp"
    mv "$BAZELISK.tmp" "$BAZELISK"
  fi
}

build_tcmalloc() {
  local workspace=$ROOT/scripts/tcmalloc
  grep -q "commit = \"$TCMALLOC_COMMIT\"" "$workspace/MODULE.bazel" ||
    die "scripts/tcmalloc/MODULE.bazel does not pin commit $TCMALLOC_COMMIT"
  local bazel_version
  bazel_version=$(<"$workspace/.bazelversion")
  local pin="tcmalloc commit:$TCMALLOC_COMMIT bazel:$bazel_version"
  local install=$PREFIX/lib/tcmalloc
  local lib=$install/libtcmalloc.so
  if up_to_date tcmalloc "$pin" "$lib"; then
    log "tcmalloc ${TCMALLOC_COMMIT:0:12} up to date"
    return
  fi
  log "tcmalloc ${TCMALLOC_COMMIT:0:12} (first build fetches abseil, re2 and protobuf; takes minutes)"
  ensure_bazelisk
  # Keep Bazel's downloads and caches under the prefix, not ~/.cache.
  local -a bazel=(env BAZELISK_HOME="$PREFIX/bazelisk" "$BAZELISK"
    --output_user_root="$PREFIX/bazel")
  local bazel_flags="-c opt --jobs=$JOBS"
  # Bazel tracks neither the compiler binary nor the system headers, and its
  # autodetected toolchain caches the compiler's include paths, so a rebuild
  # after a compiler change would reuse stale objects. Downloads survive this
  # in the repository cache.
  (cd "$workspace" && "${bazel[@]}" clean --expunge >"$PREFIX/logs/tcmalloc-clean.log" 2>&1) ||
    die "bazel clean failed; see $PREFIX/logs/tcmalloc-clean.log"
  (
    cd "$workspace"
    "${bazel[@]}" build $bazel_flags //:libtcmalloc.so >"$PREFIX/logs/tcmalloc-build.log" 2>&1
  ) || die "tcmalloc build failed; see $PREFIX/logs/tcmalloc-build.log"
  # cquery prints the output relative to Bazel's execution root.
  local execroot built
  execroot=$(cd "$workspace" && "${bazel[@]}" info $bazel_flags execution_root 2>/dev/null)
  built=$execroot/$(cd "$workspace" && "${bazel[@]}" cquery $bazel_flags --output=files //:libtcmalloc.so 2>/dev/null)
  [[ -f $built ]] || die "cannot find the built libtcmalloc.so ($built)"
  rm -rf "$install"
  mkdir -p "$install"
  cp "$built" "$lib"
  chmod u+w "$lib"
  check_symbols "$lib"
  smoke_test tcmalloc "$lib" ""
  write_info tcmalloc name=tcmalloc version="${TCMALLOC_COMMIT:0:12}" pin="$pin" \
    source="https://github.com/google/tcmalloc@$TCMALLOC_COMMIT" library="$lib" \
    buildFlags="$bazel_flags (bazel $bazel_version)" compiler="$(compiler_id)" \
    hugepageEnv=""
}

record_glibc() {
  local version
  version=$(getconf GNU_LIBC_VERSION | cut -d' ' -f2)
  mkdir -p "$PREFIX/lib/glibc"
  write_info glibc name=glibc version="$version" pin="system" source=system \
    library="" buildFlags="" compiler="" hugepageEnv="$GLIBC_HUGEPAGE_ENV"
}

write_manifest() {
  python3 -I - "$PREFIX" <<'EOF'
import json, pathlib, sys
prefix = pathlib.Path(sys.argv[1])
allocators = {}
for info in sorted(prefix.glob("lib/*/build-info.json")):
    entry = json.loads(info.read_text())
    entry.pop("pin", None)
    entry["hugepageEnv"] = dict(
        setting.split("=", 1) for setting in [entry["hugepageEnv"]] if setting)
    entry["library"] = entry["library"] or None
    allocators[entry.pop("name")] = entry
(prefix / "manifest.json").write_text(
    json.dumps({"schema": 1, "allocators": allocators}, indent=2) + "\n")
EOF
}

ensure_selftest
if wanted jemalloc; then build_jemalloc; fi
if wanted mimalloc; then build_mimalloc; fi
if wanted tcmalloc; then build_tcmalloc; fi
record_glibc
write_manifest
log "manifest: $PREFIX/manifest.json"
