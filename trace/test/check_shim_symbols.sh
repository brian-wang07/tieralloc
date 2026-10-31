#!/usr/bin/env bash
# Checks the built shim (libtieralloc-trace.so) for the properties that keep it
# from recursing into its own hooks, none of which a runtime test can see:
#
#   exports  exactly the 33 hooks and the 2 diagnostics, nothing else;
#   no-self  no PLT call or jump to an allocation entry point, which would land
#            back in a hook (or, if the shim loses the lookup, in another
#            allocator with a bootstrap pointer);
#   callees  every PLT callee is on a reviewed list of functions that don't
#            allocate on the shim's paths, so e.g. a stray snprintf fails here;
#   bindnow  linked with -z now, so ld.so's lazy binder never runs mid-hook;
#   tls      thread-locals are initial-exec (STATIC_TLS, no dynamic-TLS
#            relocations), so touching the guard's flags can't call malloc.
#
# Usage: check_shim_symbols.sh <libtieralloc-trace.so>
# Tools come from NM, OBJDUMP and READELF (default: nm, objdump, readelf).
# Exits nonzero if any check fails.

set -euo pipefail

lib=${1:?usage: check_shim_symbols.sh <libtieralloc-trace.so>}
nm=${NM:-nm}
objdump=${OBJDUMP:-objdump}
readelf=${READELF:-readelf}

hooks=(
  malloc free calloc realloc reallocarray memalign posix_memalign aligned_alloc
  valloc pvalloc malloc_usable_size free_sized free_aligned_sized
  _Znwm _Znam _ZnwmRKSt9nothrow_t _ZnamRKSt9nothrow_t
  _ZnwmSt11align_val_t _ZnamSt11align_val_t
  _ZnwmSt11align_val_tRKSt9nothrow_t _ZnamSt11align_val_tRKSt9nothrow_t
  _ZdlPv _ZdaPv _ZdlPvRKSt9nothrow_t _ZdaPvRKSt9nothrow_t _ZdlPvm _ZdaPvm
  _ZdlPvSt11align_val_t _ZdaPvSt11align_val_t
  _ZdlPvmSt11align_val_t _ZdaPvmSt11align_val_t
  _ZdlPvSt11align_val_tRKSt9nothrow_t _ZdaPvSt11align_val_tRKSt9nothrow_t
)
diagnostics=(tatrace_bootstrap_bytes tatrace_max_depth)

# Functions the shim may call through its PLT. Adding one needs a reason it
# can't allocate where the shim calls it (or, like the exception runtime, that
# any allocation it makes is an ordinary nested call through the guard).
allowedCallees=(
  # resolution: dlsym allocates only on its error path, into the bootstrap
  # buffer; dladdr and getauxval never allocate
  dlsym dladdr getauxval
  # fatal and warn
  write strlen abort
  # bootstrap copy-out, errno
  memcpy __errno_location
  # attaching the pool in init: syscall wrappers; getenv reads environ in
  # place; sysconf(_SC_PAGESIZE) reads the auxv; strerrordesc_np returns a
  # static string with no locale lookup
  getenv fstat fcntl mmap munmap close getpid getppid sysconf strerrordesc_np
  # the give-up hook's clock, read only after claimSlow's spin phase; the
  # vdso answers it in user space
  clock_gettime
  # a full ring's fallback once spinning fails: the one deliberate syscall on
  # the hook path, and it never allocates
  sched_yield
  # thread tracking: gettid once per thread when it claims a ring;
  # setspecific stores keys below 32 inside the thread (init warns past that);
  # key_create, key_delete and atfork (__register_atfork) only from init
  gettid pthread_setspecific pthread_key_create pthread_key_delete
  __register_atfork
  # operator new fallbacks: new_handler, bad_alloc, nothrow catch, unwinding
  _ZSt15get_new_handlerv __cxa_allocate_exception __cxa_throw
  __cxa_begin_catch __cxa_end_catch _Unwind_Resume _ZSt9terminatev
  # emitted by the compiler for shared-library teardown
  __cxa_finalize
)

failures=0
report() { # status name message
  printf '%s  %-9s %s\n' "$1" "$2" "$3"
  [[ $1 == FAIL ]] && failures=$((failures + 1))
  return 0
}

if [[ ! -f $lib ]]; then
  report FAIL shim "$lib not found"
  echo "FAILED: $failures check(s) failed"
  exit 1
fi

# exports
expected=$(printf '%s\n' "${hooks[@]}" "${diagnostics[@]}" | sort)
actual=$("$nm" -D --defined-only "$lib" | awk '{print $NF}' | sort)
missing=$(comm -23 <(echo "$expected") <(echo "$actual") | paste -sd' ')
extra=$(comm -13 <(echo "$expected") <(echo "$actual") | paste -sd' ')
if [[ -z $missing && -z $extra ]]; then
  report PASS exports "exactly the ${#hooks[@]} hooks and ${#diagnostics[@]} diagnostics"
else
  report FAIL exports "missing: ${missing:-none}; unexpected: ${extra:-none}"
fi

# PLT callees, from calls and tail-call jumps alike
callees=$("$objdump" -d "$lib" |
  { grep -oE '(call|jmp)[^<]*<[^>@]+@plt>' || true; } |
  sed -E 's/.*<([^>@]+)@plt>/\1/' | sort -u)

selfCalls=$(comm -12 <(printf '%s\n' "${hooks[@]}" | sort) <(echo "$callees") |
  paste -sd' ')
if [[ -z $selfCalls ]]; then
  report PASS no-self "no PLT call to any allocation entry point"
else
  report FAIL no-self "calls through the PLT: $selfCalls"
fi

unreviewed=$(comm -13 <(printf '%s\n' "${allowedCallees[@]}" | sort) <(echo "$callees") |
  paste -sd' ')
if [[ -z $unreviewed ]]; then
  report PASS callees "all $(echo "$callees" | grep -c .) PLT callees are on the reviewed list"
else
  report FAIL callees "not on the reviewed list: $unreviewed"
fi

# -z now
if "$readelf" -d "$lib" | grep -qE '\(FLAGS(_1)?\).*\bNOW\b|BIND_NOW'; then
  report PASS bindnow "linked with -z now"
else
  report FAIL bindnow "no BIND_NOW: lazy binding could run inside a hook"
fi

# initial-exec TLS. Dynamic TLS shows up as __tls_get_addr with the gnu
# dialect, but as TLSDESC relocations with gnu2 (GCC 16's default here), so
# look for the initial-exec marker and for either kind of dynamic access.
# (grep finding nothing is the good case, so it mustn't trip set -e)
dynamicTls=$("$readelf" -rW "$lib" |
  { grep -oE 'R_[A-Z0-9_]*(TLSDESC|DTPMOD)[A-Z0-9_]*' || true; } |
  sort -u | paste -sd' ')
if "$nm" -D --undefined-only "$lib" | grep -qw __tls_get_addr; then
  dynamicTls="${dynamicTls:+$dynamicTls }__tls_get_addr"
fi
if ! "$readelf" -d "$lib" | grep -qw STATIC_TLS; then
  report FAIL tls "no STATIC_TLS flag: thread-locals aren't initial-exec"
elif [[ -n $dynamicTls ]]; then
  report FAIL tls "dynamic TLS access: $dynamicTls"
else
  report PASS tls "thread-locals are initial-exec (STATIC_TLS, no dynamic TLS)"
fi

if ((failures)); then
  echo "FAILED: $failures check(s) failed"
  exit 1
fi
echo "OK: 0 check(s) failed"
