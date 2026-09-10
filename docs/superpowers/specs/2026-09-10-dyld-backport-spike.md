# Running a newer or non-Apple `dyld` on 10.9 — research spike

**Date:** 2026-09-10
**Status:** RESEARCH SPIKE. Output is an answer and a recommendation. No code, no
source changes. Nothing here has been implemented or tried beyond the throwaway
experiments recorded below.
**Question:** is there prior art for running a newer or non-Apple dynamic linker on
Mac OS X 10.9, and is it viable? A negative answer is as valuable as a positive one,
because `macho-tools` exists largely to work around 10.9's 2013-era dyld.

---

## Verdict

**Not viable as "run a modern Apple dyld on 10.9" — but a narrower option exists, and
it is more real than expected.** Nobody, anywhere, has moved a newer Apple `dyld`
onto an older macOS; every project that touches dyld's source either keeps it on its
own OS (`DiegoMagdaleno/dyld`, `PureDarwin/dyld`) or moves it sideways to Linux
(`darling-dyld`), and modern dyld is an Xcode-project-only, C++20-and-Swift,
internal-SDK build that additionally expects an OS-matched shared cache and a
`libobjc` contract 10.9's objc4-551 predates. What *is* real, and what I did not
expect, is that **10.9 sits inside a closed window in which the XNU kernel honours an
arbitrary `LC_LOAD_DYLINKER` path.** The kernel check that pins the dynamic linker to
`/usr/lib/dyld` was added in 10.11 and is absent from 10.9 and 10.10. I verified this
both in XNU source and by running a hand-written 8 KB third-party `MH_DYLINKER`,
built with this box's stock 2013 clang, as the dynamic linker for a real binary on
this 10.9.5 machine. So a replacement or shim linker is *mechanically* available to
this project in a way it is not available to anyone on a modern Mac. **I still
recommend against pursuing it**, because it would retire exactly one `macho-tools`
transform (`declassify`), would not touch the dominant real workload (repointing
`LC_LOAD_DYLIB` at stub dylibs for frameworks 10.9 does not have), and would itself
require an ahead-of-time Mach-O edit on every binary — so it trades one AOT transform
for another AOT transform *plus* a runtime component nobody has written. The premise
of `macho-tools` survives this spike intact.

---

## What I verified on this machine

Real Mac OS X 10.9.5, build 13F1911, `Darwin 13.4.0 / xnu-2422.115.15`, x86_64;
`/usr/lib/dyld` is `dyld-239.5` (from `strings`). Test binaries were built with
`/usr/bin/clang` on this box, patched with a throwaway Python script, and run. **No
system file was modified.** Two temporary files were written under `/tmp` (a path-length
constraint — see E3) and removed afterwards.

| # | Experiment | Result |
|---|---|---|
| E1 | Flip `LC_SOURCE_VERSION` (`0x2A`) to `0x8000002A` in a trivial executable, run it | `dyld: cannot load 'h_req' because it was built for OS version 10.9 (load command 0x8000002A is unknown)`, exit 133 |
| E2 | Same with `0x80000034` (the `LC_DYLD_CHAINED_FIXUPS` value) | Same message with `0x80000034`; `otool -l` prints `?(0x80000034) Unknown load command` |
| E3 | Repoint `LC_LOAD_DYLINKER` at `/tmp/mydyld`, a byte-identical copy of `/usr/lib/dyld` | Runs, exit 0 |
| E4 | Corrupt one byte of that copy (`codesign -v` → *"invalid signature (code or signature have been modified)"*), run again | Runs, exit 0 |
| E5 | Hand-write an 8 KB `MH_DYLINKER` (filetype 7) with stock 10.9 clang — `-nostdlib -Wl,-dylinker -Wl,-e,__dylinker_start -Wl,-image_base,0x7fff5f000000` — whose whole body is `exit(7)`; point a binary at it | Runs, exit **7** |
| E6 | Take the E2 binary (carrying the bogus `LC_REQ_DYLD` command `0x80000034`) and *also* point it at the custom linker | Runs, exit **7** — the kernel does not reject it |
| E7 | Custom linker pops the top of its entry stack and checks for `0xfeedfacf` | Exit **42** — the kernel hands the dylinker the main executable's `mach_header`, exactly as `_dyld_start` expects |
| E8 | Swap `LC_VERSION_MIN_MACOSX` for the same-sized `LC_SOURCE_VERSION` in a trivial C executable | Runs, exit 0 |

Notes on the experiments:

- **E5's first attempt SIGSEGV'd** at the linker's default image base `0x100000000`,
  which collides with the PIE main executable. That is an address-space collision, not
  a kernel refusal — moving the base to `0x7fff5f000000` made it work. Anyone repeating
  this should not read the crash as "the kernel blocked it".
- **E8 is a narrow observation, not a general claim.** It says only that 10.9's dyld
  will load a *trivial C executable* with no `LC_VERSION_MIN_MACOSX`. It says nothing
  about dylibs, about ObjC/AppKit binaries, or about frameworks that gate behaviour on
  the linked-on-or-after version. Do not conclude from it that `add_version_min` is
  unnecessary.

---

## Q1 — Has anyone run a newer Apple `dyld` on an older macOS?

**No documented case found.** Three projects patch Apple's dyld to build outside
Apple's internal SDK, and all three keep it on, or move it sideways from, its own OS:

| Project | What it is | Base | Maintained? |
|---|---|---|---|
| [`DiegoMagdaleno/dyld`](https://github.com/diegomagdaleno/dyld) | Apple's dyld patched to build against the *public* macOS SDK instead of internal ones, plus code commented out for unavailable symbols. Written to work around Big Sur's dylibs moving into the shared cache. | Big Sur era | **No** — archived 2021-10-31; README: *"This is old software and untested on other Macs, run at your discretion."* |
| [`PureDarwin/dyld`](https://github.com/PureDarwin/dyld) | Apple's source plus build fixes ("Remove references to macosx.internal SDK"). Import commits are literally `dyld-519.2.2` and `dyld-655.1.1`. | dyld-655 = **macOS 10.14**, which predates chained fixups entirely | **No** — last code push 2020-07-20; repo archived 2026-07-18. (The PureDarwin *org* is alive — `PureDarwin/PureDarwin` pushed 2026-09-09 — the dyld component specifically is not.) |
| [`darlinghq/darling-dyld`](https://github.com/darlinghq/darling-dyld) | Apple's dyld patched to run on **Linux**. README names its upstream as `apple-oss-distributions/dyld/tree/dyld-852.2`. | dyld-852.2 = macOS 12, so it *does* have chained fixups | Last push 2024-06-23 |

The direction-of-travel trap the brief warns about is exactly what happened here: all
the "legacy" energy is *old apps on new macOS* or *new macOS on old hardware*.
Moving a dyld **backwards** onto an older OS has, as far as I can find, never been
attempted by anyone.

`darling-dyld` is the most interesting of the three for us, because it is a
known-buildable, de-internal-SDK'd **chained-fixups-aware** dyld. Its patches target
Linux, not old Darwin, so they are not directly reusable — but they are proof that
dyld-852-era source can be made to build and run outside Apple's build system.

---

## Q2 — Has anyone written a third-party / replacement dynamic linker for macOS?

Yes, but only barely, and nothing usable here.

**Actual third-party `MH_DYLINKER` producers — exactly one found:**

- [`pauldcs/dynld`](https://github.com/pauldcs/dynld) — "OSX dynamic linker written in
  Rust". Its README shows `file` reporting `Mach-O 64-bit dynamic linker arm64`, i.e.
  filetype 7. Runs without libSystem, rebases itself, uses raw Mach traps, walks the
  dyld shared cache for symbols. **arm64/arm64e only; no Objective-C; no mention of
  chained fixups; hobby scale (4 stars); last push 2026-06-18.** It ships a
  `tools/launcher` that "replicates how the kernel would invoke the dynamic linker
  during an `execve`" — *because on modern macOS the kernel refuses to honour its
  `LC_LOAD_DYLINKER`.* On 10.9.5 that launcher would be unnecessary (see Q4).

**Loaders that are not dylinkers:**

- [`darlinghq/darling`](https://github.com/darlinghq/darling/blob/master/src/startup/mldr/mldr.c)'s
  **`mldr`** — the architectural precedent for the "shim" idea in Q5. It is an
  ordinary **Linux ELF** program that maps the Mach-O, then loads Apple's real dyld
  and jumps into it (`static const char* dyld_path = INSTALL_PREFIX "/libexec/usr/lib/dyld";`).
  Darling *abandoned* full reimplementation: its predecessor
  [`shinh/maloader`](https://github.com/shinh/maloader) was a genuine from-scratch
  userland Mach-O loader, and the 2017
  ["Mach-O transition"](https://blog.darlinghq.org/2018/07/mach-o-linking-and-loading-tricks.html)
  replaced it with the thin-loader-plus-real-dyld design.
- [`pauldcs/macho-loader-rs`](https://github.com/pauldcs/macho-loader-rs) — in-memory
  reflective Mach-O loader in Rust that **explicitly supports chained fixups**; README
  says build with `-macosx-version-min=12.0`. Runs inside an existing process, not as a
  dylinker.
- [`ogre2007/compatra`](https://github.com/ogre2007/compatra) — Rust + Unicorn Engine,
  runs arm64 Mach-O binaries on Intel macOS via a "no-dyld runner" with its own loader.
  The one project found whose *direction of travel* resembles ours. GPL-2.0, 1 star,
  pushed 2026-06-27. Chained-fixups support **unverified**.
- [`wie-project/kakehashi`](https://github.com/wie-project/kakehashi) — `kh-loader`
  crate does Mach-O parse/map/bind/execute from scratch. **Explicitly unmaintained**
  per its README; Linux aarch64 execution only, macOS gets "dry-load" mode.
- XPN's ["Building a Custom Mach-O Memory Loader for macOS"](https://blog.xpnsec.com/building-a-mach-o-memory-loader-part-1/)
  series covers writing a loader that handles `LC_DYLD_CHAINED_FIXUPS`. *The page
  returned HTTP 403 to my fetch; this is at search-snippet confidence only.*

**ELF-world analogues ported to Mach-O — one real case:** ravynOS modified FreeBSD's
`rtld-elf` into a hybrid runtime linker **installed as `/usr/lib/dyld`**, parsing Mach
headers, `LC_MAIN` and `LC_LOAD_DYLIB`, interpreting Mach-O bind opcodes against ELF
symbol tables ([writeup](https://gist.github.com/mszoek/2916926a57011bc369e0431561f3d5f7)).
But on 2025-10-29 ravynOS
[decided to abandon FreeBSD and rebase on Darwin/XNU](https://github.com/ravynsoft/ravynos/discussions/529)
precisely to stop maintaining ports of `ld64`/`cctools`/`dyld`. The hybrid rtld is
now a historical artifact. **No port of glibc's or musl's `ld.so` to Mach-O was
found.**

**Jailbreak world: nothing that is a linker.** ElleKit, libhooker, litehook are
function hookers; [`opa334/ChOma`](https://github.com/opa334/ChOma) is a Mach-O and
code-signature *file format* library (active, 444 stars — genuinely interesting as a
Mach-O rewriting library, irrelevant as a loader). "ipa-loader" surfaced nothing.
**Go and Zig: nothing.** Zig has a Mach-O *static* linker, not a dylinker.

---

## Q3 — How hard are the specific obstacles?

Assessed for the "build modern Apple dyld and run it on 10.9" plan.

### Buildability — **hard, and the reason nobody has done it**

`apple-oss-distributions/dyld` at `main` ships **only `dyld.xcodeproj`** — no CMake,
no Makefile. There are 21 top-level source directories and a `configs/` directory of
50-plus `.xcconfig` files. From
[`configs/base.xcconfig`](https://github.com/apple-oss-distributions/dyld/blob/main/configs/base.xcconfig)
(verified):

```
GCC_C_LANGUAGE_STANDARD = c2x
CLANG_CXX_LANGUAGE_STANDARD = c++20
SWIFT_VERSION = 5.0
OTHER_SWIFT_FLAGS = -enable-experimental-feature BorrowingSwitch -enable-experimental-feature Lifetimes -enable-experimental-feature LifetimeDependence
```

So modern dyld is **C2x + C++20 + Swift 5 with experimental compiler features**, built
by Xcode, against Apple-internal SDKs. 10.9's clang cannot compile any of it;
`mavericks-clang`'s clang-22 could handle the C and C++, and there is no Swift
toolchain for 10.9 at all.

The historically documented blocker is narrower and older: Apple DTS confirmed on the
[developer forums](https://developer.apple.com/forums/thread/54331) that building
dyld-360.18 fails on `_simple_getenv`, which **"has moved to libplatform, which is not
open source"**. That specific one is *merely work* — `libplatform` has since been
open-sourced and a reference implementation is
[public](https://opensource.apple.com/source/libplatform/libplatform-161/src/simple/getenv.c.auto.html).
The thread contains **no successful build and no discussion of deploying a custom
dyld**. Both `DiegoMagdaleno/dyld` and `PureDarwin/dyld` exist precisely because
de-internal-SDK-ing dyld is a project in itself.

**Verdict: work, not a hard block, for the C/C++ parts — but a large amount of it, and
the Swift is a hard block for whatever depends on it.** (Which components need Swift:
**unverified**.)

### Shared cache — **likely a hard block; not fully determined**

10.9's cache is `/private/var/db/dyld/dyld_shared_cache_x86_64`, 335 MB, magic
`dyld_v1  x86_64` (verified locally). Modern dyld's cache format is versioned and
built by the OS's own `dyld_shared_cache_builder` — the repo carries a whole
`cache_builder/` tree and a `shared_cache_runtime/`. **Whether modern dyld has a
supported cacheless path, and whether it would reject a `dyld_v1  x86_64` cache
outright, I did not determine.** This is the single obstacle I am least sure about and
it is potentially decisive.

### Objective-C runtime coupling — **hard block for anything touching ObjC**

Modern dyld drives `libobjc` through `_dyld_objc_notify_register` and does class and
selector pre-optimisation; 10.9 ships objc4-551, which predates that contract. Any
modern-dyld-on-10.9 plan would have to run against 10.9's `libobjc` and would find the
interface it expects absent. **Exact signature-by-version comparison: unverified.**
Note that `macho-tools` already has *two* verbs (`segment`, `retag-swift`) whose whole
job is placating 10.9's `libobjc` — that coupling is real and is not a dyld problem.

### Code signing / AMFI — **not a block. Verified the other way.**

The brief's instinct was right: 10.9 predates SIP and is *more* permissive. E4 shows a
`/tmp`-resident, user-owned dyld whose signature `codesign -v` calls *"invalid
signature (code or signature have been modified)"* being used as the process's dynamic
linker with exit 0. E5 shows an entirely unsigned, hand-built one doing the same.

### `dyld_all_image_infos` / the debugger interface — **a real requirement, cheaply met**

The 10.9 kernel finds the linker's all-image-info block itself. From
[`xnu-2422.115.4/bsd/kern/mach_loader.c`](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-2422.115.4/bsd/kern/mach_loader.c),
`note_all_image_info_section()` (verified):

```c
if (strncmp(scp->segname, "__DATA", sizeof(scp->segname)) != 0)
        return;
...
if (0 == strncmp(sectionp->s64.sectname, "__all_image_info", ...)) {
        result->all_image_info_addr = ...;
```

So **any replacement dylinker must carry a `__DATA,__all_image_info` section** in the
layout 10.9's debugger, `libproc`, `dtrace` and crash reporter expect, or those all
break. That is a constraint on a shim, not a blocker — but it is one that E5's 8 KB toy
does not satisfy and a real shim would have to.

### Syscall / kernel ABI — **not determined**

Modern dyld reaches the kernel through `libsystem_kernel` internals. I did **not**
enumerate which modern traps and `mach_vm_*`/`task_info`/`os_unfair_lock`/page-in-linking
facilities xnu-2422 lacks. Assume it is substantial and assume it is work rather than a
clean block, but **treat this as unverified**.

---

## Q4 — Is `LC_LOAD_DYLINKER` redirection actually viable on 10.9?

**Yes. Verified twice, independently, and this is the spike's most surprising result.**

The kernel check that pins the dynamic linker to `/usr/lib/dyld` **was added in 10.11
and does not exist in 10.9 or 10.10.** Grepping `bsd/kern/mach_loader.c` across XNU
releases (verified — I fetched each file and grepped it):

| XNU | macOS | `DEFAULT_DYLD_PATH` check in `load_dylinker()` |
|---|---|---|
| `xnu-2422.115.4` | **10.9.5** | **absent** |
| `xnu-2782.1.97` | 10.10 | **absent** |
| `xnu-3247.1.106` | 10.11 | **present** |
| `xnu-11215.1.10` | macOS 15 | present |

From [xnu-3247.1.106](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-3247.1.106/bsd/kern/mach_loader.c) onward:

```c
#if !(DEVELOPMENT || DEBUG)
	if (0 != strcmp(name, DEFAULT_DYLD_PATH)) {
		return (LOAD_BADMACHO);
	}
#endif
```

10.9's `load_dylinker()` takes the path straight out of the load command with no such
validation. This means the universally repeated folklore — including Quinn "The
Eskimo!" of Apple DTS, who writes that *"Apple platforms have vestigial support for
custom dynamic linkers (your executable tells the system which dynamic linker to use
via the `LC_LOAD_DYLINKER` load command). This facility originated on macOS's ancestor
platform and has never been a supported option on any Apple platform"*
([Apple Developer Forums](https://developer.apple.com/forums/thread/715385)) — is
**true for 10.11 and later and factually false for 10.9**. Unsupported, yes. Blocked,
no. E3–E7 confirm it end to end on this box.

Constraints the 10.9 kernel *does* impose, from `parse_machfile()` in xnu-2422
(verified in source, and consistent with E5):

- The replacement must be `filetype == MH_DYLINKER` (7); any other filetype at
  `depth == 2` returns `LOAD_FAILURE`.
- It is loaded at `depth == 2` and is always ASLR-slid
  (`if ((header->flags & MH_PIE) || (header->filetype == MH_DYLINKER)) slide = aslr_offset;`).
- It should provide `__DATA,__all_image_info` (see Q3).

And one practical constraint that is *this repo's business*: the path lives inside the
existing `LC_LOAD_DYLINKER` command. A stock command for `/usr/lib/dyld` is
`cmdsize 32` with `name offset 12`, leaving **20 bytes — a replacement path of at most
19 characters**. `/usr/lib/dyld9` fits; `/usr/local/lib/dyld9` does not. Anything
longer needs header room, i.e. `macho9 grow`.

---

## Q5 — Is there a narrower option than a whole dyld?

**Yes, and it is a real design rather than a fantasy — but it is not worth doing.**

The brief flags a possible objection: that `LC_REQ_DYLD` might cut against a shim.
**It does not.** E6 settles it: a binary carrying the unknown required load command
`0x80000034` runs to completion under a replacement dylinker. The refusal is purely
dyld's, in `ImageLoaderMachO::parseLoadCmds()`
([source](https://github.com/opensource-apple/dyld/blob/master/src/ImageLoaderMachO.cpp)):

```c++
default:
    if ( (cmd->cmd & LC_REQ_DYLD) != 0 ) {
        if ( firstUnknownCmd == NULL )
            firstUnknownCmd = cmd;
    }
```
```c++
if ( firstUnknownCmd != NULL ) {
    if ( minOSVersionCmd != NULL )
        dyld::throwf("cannot load '%s' because it was built for OS version %u.%u (load command 0x%08X is unknown)", ...);
    else
        dyld::throwf("cannot load '%s' (load command 0x%08X is unknown)", ...);
```

Both format strings are present verbatim in this box's `/usr/lib/dyld` binary
(`strings -a` — verified locally), and E1/E2 reproduce them exactly. **The brief's
load-bearing claim is confirmed with one important refinement: the refusal is
dyld-level, not kernel-level.** "No code of ours can run first" is true only while
`/usr/lib/dyld` is the linker. Redirect `LC_LOAD_DYLINKER` and *our* code runs first
and is the thing reading the command.

So the shim is buildable in principle, and the architecture has a working precedent in
Darling's `mldr` (map the image, load the real linker, hand off). A 10.9 version would:

1. be an `MH_DYLINKER` at a non-colliding image base, with `__DATA,__all_image_info`;
2. receive the main executable's `mach_header` on the stack (E7 confirms the kernel
   provides it);
3. `mach_vm_protect` the mapped header to writable, lower the chained fixups in place,
   rewrite the load commands to `LC_DYLD_INFO_ONLY`, restore protection;
4. map `/usr/lib/dyld`'s segments itself, apply its slide, reconstruct the entry stack,
   and jump to its `_dyld_start`.

Steps 3 and 4 are each substantial, and step 3 is strictly harder in memory than on
disk: the opcode streams `declassify` appends have to live *somewhere a segment
covers*, which on disk means extending `__LINKEDIT` and in memory means allocating and
re-pointing. **And the killer:** step 0 is patching `LC_LOAD_DYLINKER` in the vendor
binary — an ahead-of-time Mach-O edit, possibly needing `grow` for the 19-character
path limit. The shim does not remove an AOT pass. It adds a runtime component *behind*
one.

---

## Q6 — What would it actually buy?

Very little. Itemised against the current toolset.

### Would be retired by a chained-fixups-capable linker

| Verb / tool | Why it would go |
|---|---|
| `macho9 declassify` / `patch_macho` | Its entire job is lowering `LC_DYLD_CHAINED_FIXUPS` to `LC_DYLD_INFO_ONLY`. A linker that reads chained fixups makes it unnecessary. |

That is the complete list.

### Would **not** be retired

| Verb / tool | Why it survives |
|---|---|
| `macho9 dylib` / `rpath` / `change_dylib` | **The dominant real workload.** Repointing `LC_LOAD_DYLIB` at stub dylibs for CoreSpotlight, Metal, UserNotifications and friends. No dynamic linker can conjure a framework 10.9 does not ship. |
| `macho9 segment` / `rename_segment` | `__DATA_CONST` → `__DATA` exists because **10.9's `libobjc` (objc4-551)** looks for ObjC metadata in `__DATA`. That is a runtime library problem, not a linker problem. |
| `macho9 retag-swift` / `retag_swift_classes` | Moves the is-Swift tag from the stable-ABI bit to the legacy one — again for 10.9's `libobjc`. |
| `macho9 grow` | Makes header room. Needed *more*, not less, under the shim design, because redirecting `LC_LOAD_DYLINKER` to anything over 19 characters requires it. |
| `macho9 lc` (`-strip-lc`) | Same reason. |
| `macho9 minos` / `add_version_min` | Not a dyld *load* gate for a trivial executable (E8), so a new linker would not retire it; what it is actually for lives above dyld. |
| `fix_macho` | Install names and build-version stripping across fat binaries. Orthogonal. |
| `mg_verify` / `mg_plausible` | Verification of this repo's own edits. Orthogonal. |

**One transform out of eleven.** And to get it you would have to write, ship, and
support a dynamic linker — the single most safety-critical component in the process —
for an OS with no CI runner, when the alternative is a deterministic file transformer
that is already written, already gated by two independent verifiers, and produces a
binary that the *stock, Apple-signed, well-tested* `dyld-239.5` then loads normally.
The AOT approach is not merely cheaper. It is the one with the smaller blast radius.

---

## Recommendation

1. **Do not pursue a dyld backport, fork, or shim.** Keep `declassify`. Nothing in
   this spike weakens the case for `macho-tools`; Q6 strengthens it.
2. **Record the `LC_LOAD_DYLINKER` window as a fact**, because it is genuinely
   non-obvious, it is contradicted by every source a future reader will find, and it
   expires at 10.11. If a future need arises that *only* a custom linker can serve —
   something needing to run code before the image is bound — this document says the
   door is open on 10.9 and 10.10 and shows how to check.
3. **Do not use `LC_LOAD_DYLINKER` redirection as a supported `macho-tools` verb** on
   the strength of this. There is no consumer for it, and adding a verb for a
   hypothetical is the opposite of this repo's discipline.
4. One incidental finding is worth acting on and belongs to the sibling repo — see
   [`mavericks-cctools/docs/superpowers/specs/2026-09-10-dyld-findings.md`](../../../../mavericks-cctools/docs/superpowers/specs/2026-09-10-dyld-findings.md).

---

## What I could not determine

Stated explicitly, because a confident wrong answer here is worse than a gap.

1. **Whether modern dyld can run without a `dyld_shared_cache`**, and whether it would
   reject 10.9's `dyld_v1  x86_64` cache or merely mis-parse it. I checked the local
   cache's magic and saw that the repo carries a `cache_builder/`; I did not read the
   runtime's cache-validation path. **This is the biggest gap and it may be decisive.**
2. **Which modern syscalls, Mach traps and kernel facilities modern dyld needs that
   xnu-2422 lacks.** Not enumerated at all.
3. **Which parts of modern dyld require Swift**, and therefore how much of it is
   categorically unbuildable for 10.9 rather than merely laborious.
4. **The exact `_dyld_objc_notify_register` signature drift** between dyld-239 and
   modern dyld, version by version.
5. **Whether `dynld` or `compatra` handle `LC_DYLD_CHAINED_FIXUPS`.** Neither README
   says.
6. **Whether 10.9 imposes any code-signing constraint on the dylinker vnode beyond
   `parse_machfile()`'s filetype and depth checks.** E4 and E5 are strong evidence that
   it does not for an ordinary user binary, but I did not audit `get_macho_vnode()`.
7. **Whether Rust can still target x86_64 macOS 10.9**, which bears on whether
   `dynld` is even portable here in principle.
8. XPN's memory-loader series — **the site returned HTTP 403 to my fetch.** Cited at
   search-snippet confidence only.

---

## Sources

- [apple-oss-distributions/dyld](https://github.com/apple-oss-distributions/dyld) — `configs/base.xcconfig`, `mach_o/`, `mach_o_writer/`, `other-tools/`, tags
- [apple-oss-distributions/xnu — `xnu-2422.115.4/bsd/kern/mach_loader.c`](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-2422.115.4/bsd/kern/mach_loader.c) (10.9.5)
- [apple-oss-distributions/xnu — `xnu-3247.1.106/bsd/kern/mach_loader.c`](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-3247.1.106/bsd/kern/mach_loader.c) (10.11, where the restriction lands)
- [opensource-apple/dyld — `src/ImageLoaderMachO.cpp`](https://github.com/opensource-apple/dyld/blob/master/src/ImageLoaderMachO.cpp) — `parseLoadCmds()` and the `LC_REQ_DYLD` refusal
- [Apple Developer Forums — "An Apple Library Primer" (Quinn, DTS)](https://developer.apple.com/forums/thread/715385) — custom dynamic linkers "never a supported option"
- [Apple Developer Forums — "Trying to compile dyld"](https://developer.apple.com/forums/thread/54331) — `_simple_getenv`, libplatform, no successful build
- [DiegoMagdaleno/dyld](https://github.com/diegomagdaleno/dyld) · [PureDarwin/dyld](https://github.com/PureDarwin/dyld) · [darlinghq/darling-dyld](https://github.com/darlinghq/darling-dyld)
- [darlinghq/darling — `src/startup/mldr/mldr.c`](https://github.com/darlinghq/darling/blob/master/src/startup/mldr/mldr.c) · [Darling loader docs](https://docs.darlinghq.org/internals/basics/loader.html) · [Mach-O linking and loading tricks](https://blog.darlinghq.org/2018/07/mach-o-linking-and-loading-tricks.html) · [shinh/maloader](https://github.com/shinh/maloader)
- [pauldcs/dynld](https://github.com/pauldcs/dynld) · [pauldcs/macho-loader-rs](https://github.com/pauldcs/macho-loader-rs) · [ogre2007/compatra](https://github.com/ogre2007/compatra) · [wie-project/kakehashi](https://github.com/wie-project/kakehashi)
- [ravynOS hybrid rtld-elf writeup](https://gist.github.com/mszoek/2916926a57011bc369e0431561f3d5f7) · [ravynOS discussion #529 (abandoning FreeBSD)](https://github.com/ravynsoft/ravynos/discussions/529)
- [opa334/ChOma](https://github.com/opa334/ChOma) · [tealbathingsuit/ellekit](https://github.com/tealbathingsuit/ellekit)
- [qyang-nj/llios — chained_fixups.md](https://github.com/qyang-nj/llios/blob/main/dynamic_linking/chained_fixups.md) · [dyld/include/mach-o/fixup-chains.h](https://github.com/apple-oss-distributions/dyld/blob/main/include/mach-o/fixup-chains.h)
- [libplatform-161 `getenv.c`](https://opensource.apple.com/source/libplatform/libplatform-161/src/simple/getenv.c.auto.html)
- [XPN — Building a Custom Mach-O Memory Loader, part 1](https://blog.xpnsec.com/building-a-mach-o-memory-loader-part-1/) *(403 on fetch; snippet-level only)*
