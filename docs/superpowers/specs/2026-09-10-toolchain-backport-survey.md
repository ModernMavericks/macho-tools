# Toolchain-backport survey: what exists, what replaces `macho-tools`, what to package

Research date: 2026-09-10. Method: web search plus direct reads of upstream source
files, Portfiles, READMEs and the GitHub API. Every claim is tagged **verified** (read
at the cited URL, quoted where it matters) or **inferred** (not checked directly).
Where a project states a macOS floor, it is quoted.

---

## Summary

**The state of the art.** This space is genuinely thin, and thin in a specific,
diagnosable way. There is a real, maintained ecosystem for *cross-compiling to* old
macOS — osxcross, cctools-port's Linux/BSD path, MacPorts' legacy Portfiles, the SDK
collections — and almost nothing for running a *modern* toolchain *on* an old macOS
host. The three big adjacent communities are all pointed elsewhere: OpenCore Legacy
Patcher runs new macOS on old hardware and, verified by reading `sys_patch.py`, works by
swapping whole files out of a prebuilt payload rather than parsing Mach-O at all;
Retroactive runs old apps on new macOS and does its one piece of Mach-O surgery by
calling `insert_dylib`; the Procursus/theos jailbreak world rebuilds a great deal of
Apple tooling but bottoms out at Big Sur. The most telling data point is MacPorts, the
project most philosophically committed to old Darwin: its own `devel/ld64` Portfile
defaults Darwin 13 to `ld64-274` (Xcode 8.2.1 era), and even its `ld64-latest` is 450.3
(Xcode 10.2, 2019) — all of it pre-chained-fixups. `iains/darwin-xtools` proves cctools
and ld64 *can* be built natively on Darwin 13, but it stops at cctools-906/ld64-351.8
(2018). Against that backdrop, the single most valuable thing this survey found is that
**`tpoechtrager/cctools-port`'s `master` branch builds and runs natively on macOS 10.9
today**, verified by reading the working recipe in `Wowfunhappy/WebKit@mavericks-backport`
— twelve cctools binaries built on a 10.9 host and then each exercised against
`/usr/lib/dyld` as a build gate. Nobody has packaged it.

**Is `macho-tools` redundant?** Partly, and more than the repo currently assumes — but
not at its core. Two of its six verbs have real, in-place, cctools-native equivalents I
verified in source. `add_version_min` is subsumed by `vtool -set-version-min`, whose
`command_set` comment states it "does not change the file's size in any way… leaving the
broader structure of the Mach-O completely unchanged. if the new load commands do not fit
in the existing space between the mach_header and the first segment, command_set will
return an error" — Apple already implements the never-move-a-byte discipline, and `vtool`
does not go through `breakout`/`checkout`/`writeout`, so it has no `__LINKEDIT` ordering
gate at all. And `fix_macho`'s install-name rewriting is largely `install_name_tool
-change`/`-id`, because modern `libstuff/checkout.c` *does* handle `LC_DYLD_CHAINED_FIXUPS`
and `LC_DYLD_EXPORTS_TRIE` — 10.9's refusal is an artefact of its tool's age, not a
property of the binaries. But the two hardest requirements survive intact. **Nothing
anywhere lowers chained fixups to `LC_DYLD_INFO_ONLY`** — two independent searches found
only parsers (LIEF, the Binary Ninja and Rizin plugins, `dyld_info`) and the universal
advice to relink with `-no_fixup_chains`, which a vendor binary forecloses. And `-grow`
has no equivalent: `install_name_tool` hits the same wall and says so in its error text —
"the program must be relinked, and you may need to use -headerpad or
-headerpad_max_install_names" — which is exactly the dead end `-grow` exists to route
around.

**The top packaging candidate is cctools.** Every port in the family that touches a Mach-O
today reaches for `/usr/bin/otool` or `/usr/bin/install_name_tool` and gets a 14K xcselect
shim, or reaches for `/usr/bin/dyldinfo` and gets nothing. `mavericks-clang` ships clang
and lld (verified: `-DCLANG_DEFAULT_LINKER=lld`) and no binutils at all, so the gap is
family-wide and unowned. The work is de-risked to an unusual degree because a complete,
self-verifying recipe already exists in the WebKit branch: one upstream pin, one small
patch, two polyfills the family already ships, three `configure` flags, seven `make`
targets, and a deliberate exclusion of `ld`. Packaging it converts a per-port chore into
a family product — and it is the prerequisite for honestly answering the redundancy
question above, since you cannot retire `add_version_min` in favour of `vtool` until
`vtool` is something a 10.9 machine has.

---

## Prior art

| Project | URL | What it does | macOS floor | Maintained? | Verified? |
|---|---|---|---|---|---|
| `tpoechtrager/cctools-port` (**`master` branch**) | https://github.com/tpoechtrager/cctools-port | cctools 973.0.1 / ld64 609: otool, lipo, install_name_tool, nm, nmedit, strip, size, strings, libtool, ar, ranlib, **vtool**, seg_hack, dyldinfo | README: "Clang 3.4 or later"; **builds and runs on 10.9** with 2 shims, 1 patch, `ld` excluded | Yes — tip commit 2024-09-14; repo push 2026-04-03 | Verified (source + WebKit recipe + branch API) |
| `tpoechtrager/cctools-port` (**default branch** `1030.6.3-ld64-956.6`) | same | Newer cctools/ld64, chained-fixups-aware linker | README: "Clang 10+, libstdc++ or libc++ with C++20 support… libdispatch-dev and libblocksruntime" — **not bootstrappable on 10.9** | Yes | Verified (README) |
| `Wowfunhappy/WebKit@mavericks-backport` | https://github.com/Wowfunhappy/WebKit/tree/mavericks-backport/MavericksSupport | Builds clang-22, cctools, git, python3, ruby, nasm, ninja, cmake **natively on 10.9**; polyfill layer; SDK patches | 10.9 host, 10.9 deployment target, modern SDK | Yes — active | Verified (read `build_cctools.sh` + README) |
| `iains/darwin-xtools` | https://github.com/iains/darwin-xtools | cctools + ld64 modified "to enable support for older systems with up-to-date-tools"; used on "Darwin9…Darwin14 (OS X 10.5+)" as a **build host** | 10.5+; needs only cmake 3.4.1 and a C++11 compiler | Repo push 2026-07-09, but version ceiling frozen at cctools-906/ld64-351.8 (2018) | Verified (README, branches) |
| MacPorts `devel/ld64` | https://github.com/macports/macports-ports/blob/master/devel/ld64/Portfile | Five Darwin-gated ld64 subports | **Darwin 13 default is `ld64-274`**; `ld64-latest` is 450.3 (Xcode 10.2) — all pre-chained-fixups | Yes | Verified (Portfile, quoted logic `if {${os.major} < 15} { default_variants +ld64_274 }`) |
| MacPorts `devel/cctools` | https://github.com/macports/macports-ports/blob/master/devel/cctools/Portfile | cctools port | Comment: on `${os.major} >= 12`, "llvm-3.7 is the newest llvm the system toolchain can build on these systems" | Yes | Verified (Portfile comments) |
| `tpoechtrager/osxcross` | https://github.com/tpoechtrager/osxcross | Linux/BSD → macOS cross toolchain wrapping cctools-port + SDKs | README: "Host OSes: Linux, \*BSD" — **macOS is not a supported host** | Yes — push 2026-07-24, 3392 stars | Verified (README) |
| `tpoechtrager/apple-libtapi` | https://github.com/tpoechtrager/apple-libtapi | Apple TAPI (`.tbd` stubs), sliced from a 2024 LLVM snapshot | Not stated; inherits LLVM's build floor | Yes — push 2026-08-19 | README verified; floor inferred |
| `macports/macports-legacy-support` | https://github.com/macports/macports-legacy-support | libc/libSystem gap-fills; per-function "Max Version Needing Feature" table spanning 10.4–10.14 | Old systems by design; **confirmed built natively on 10.9** | Yes — push 2026-09-09 | Verified (README + https://topanswers.xyz/nix?q=1103) |
| `theos/theos` | https://theos.dev/docs/installation-macos | Jailbreak build system | **"Minimum OS version: macOS Mavericks (10.9)"** — but via *stock Xcode 5*, not modern tooling | Yes — push 2026-08-08 | Verified (quoted) |
| `ProcursusTeam/Procursus` | https://github.com/ProcursusTeam/Procursus | Rebuilds cctools, ld64, ldid for macOS/iOS/tvOS | macOS 11+; `ldid-procursus` bottles only 13/14+, MacPorts port build-depends clang-18 | Yes — push 2026-09-09 | Bottle/port evidence verified; docs site returned HTTP 402 |
| `ProcursusTeam/ldid` | https://github.com/ProcursusTeam/ldid | Fake/real code signatures in a Mach-O | Not stated; Procursus fork needs OpenSSL 3 | Yes — push 2026-07-24 | Activity verified; 10.9 buildability **inconclusive** |
| `indygreg/apple-platform-rs` (`rcodesign`) | https://github.com/indygreg/apple-platform-rs | Cross-platform Apple code signing | **Cannot run on 10.9** — Rust's floor is "macOS: 10.12 Sierra" since Rust 1.74 | Yes — push 2026-08-29 | Verified (https://blog.rust-lang.org/2023/09/25/Increasing-Apple-Version-Requirements/) |
| `dortania/OpenCore-Legacy-Patcher` | https://github.com/dortania/OpenCore-Legacy-Patcher | Root-volume patching by **whole-file swap** from a prebuilt payload | App requires 10.10+; targets Big Sur–Sequoia | Yes — v2.5.0, 2026-09-08 | Verified (read `sys_patch.py`) |
| `cormiertyshawn895/Retroactive` | https://github.com/cormiertyshawn895/Retroactive | Old Apple apps on new macOS; injects a dylib via `insert_dylib` | Modern macOS; opposite direction | Winding down — README calls the next release "likely the final version" | Verified (author's Medium deep-dive) |
| `Tyilo/insert_dylib` | https://github.com/Tyilo/insert_dylib | Appends one `LC_LOAD_DYLIB`; optionally strips the code signature | Not stated | Low activity — push 2025-03-29, 2097 stars | Verified (README + API) |
| `alexzielenski/optool` | https://github.com/alexzielenski/optool | Insert/remove load commands, strip/resign signature, clear PIE | Not stated | **Dead** — push 2019-09-06 | Verified (API) |
| `lief-project/LIEF` | https://github.com/lief-project/LIEF | Full multi-format parse/edit library; parses chained fixups **and** classic dyld info | Modern C++/CMake; will not build with 2013 clang | Very active — push 2026-09-06, v1.0.0 on 2026-07-12 | Verified (`Builder.hpp`, docs) |
| `jtool` / `jtool2` / `disarm` | https://www.newosxbook.com/tools/jtool.html | Inspector/disassembler; narrow entitlement + self-sign edits | N/A | **Deprecated by author**; "Free, yes. But not open source." | Verified (quoted) |
| `gdbinit/MachOView` | https://github.com/gdbinit/MachOView | Mach-O viewer | **Xcode 13+, deployment target 10.13+** | Frozen — README: "This repo is frozen in time" | Verified (README) |
| `phracker/MacOSX-SDKs` | https://github.com/phracker/MacOSX-SDKs | "MacOSX10.1.5.sdk thru MacOSX11.3.sdk" — **includes 10.9** | N/A | **Stale** — push 2022-11-11 | Verified (API + README) |
| `alexey-lysiuk/macos-sdk` | https://github.com/alexey-lysiuk/macos-sdk | Apple macOS SDKs | N/A | Yes — push 2026-07-02 | Verified (API) |
| `devernay/xcodelegacy` | https://github.com/devernay/xcodelegacy | "Legacy components for XCode 4-12 (deprecated compilers and Mac OS X SDKs)" | N/A | Yes — push 2026-07-23 | Verified (API) |
| `keith/cctools` | https://github.com/keith/cctools | Buildable mirrors of Apple cctools drops | `buildable-*` branches only back to Xcode 12 | Yes — push 2026-06-12 | Verified (README, branches) |
| `apple-oss-distributions/dyld` | https://github.com/apple-oss-distributions/dyld | Apple's live dyld source | N/A — no old-OS story | Yes (Apple) | Verified; **no dyld backport project found anywhere** |
| Homebrew | https://docs.brew.sh/Support-Tiers | Package manager | **"macOS 10.15 (Catalina) and older will not run Homebrew at all."** | Yes | Verified (quoted) |
| LLVM libc++ | https://discourse.llvm.org/t/minimum-macos-deployment-target-increases-to-11-0-in-v22-1-visibility-discussion-on-update-policy/89751 | C++ standard library | **Minimum macOS deployment target rises 10.13 → 11.0 in LLVM v22.1** | Yes | Verified (see risk note below) |

---

## Does anything replace `macho-tools`?

The honest answer is verb by verb, not repo by repo. Two of six verbs have real
replacements; the two hardest do not, and nothing surveyed is close.

### The two hard requirements, checked first

**Never move a byte.** This turns out *not* to be unique, which is worth knowing.
Apple's own cctools already enforces it in two places I read:

- `cctools/misc/vtool.c`, `command_set`: "command_set does not change the file's size in
  any way. instead, it will rewrite the contents of the file in memory, leaving the
  broader structure of the Mach-O completely unchanged. if the new load commands do not
  fit in the existing space between the mach_header and the first segment, command_set
  will return an error." (verified)
- `cctools/misc/install_name_tool.c`, ~line 674: if `new_sizeofcmds + sizeof_mach_header
  > low_fileoff`, it errors with "larger updated load commands do not fit (the program
  must be relinked, and you may need to use -headerpad or -headerpad_max_install_names)".
  (verified)

So "edit within the pad or refuse" is standard Apple discipline. What is *not* standard is
`-grow`: lowering the image base to manufacture pad when the pad has run out. Apple's
answer at that point is literally "the program must be relinked", which is the one thing a
200MB vendor binary forecloses. **`-grow` has no equivalent anywhere in this survey** —
verified for cctools, inferred-absent elsewhere from two independent negative searches.

One caveat in the other direction: `install_name_tool` goes through `breakout` →
`checkout` → `writeout`, and `writeout` re-emits the file with recomputed symbol-info
sizes. It does not move *segment* data, but it is not the byte-preserving edit `macho9`
performs, and it will drop or invalidate a code signature. `vtool` does not use that path
at all — verified from its includes, which pull `mach-o/loader.h` and `stuff/port.h` and
nothing from `stuff/breakout.h`.

**Chained-fixups lowering.** Nothing. This is the clearest result in the survey, reached
independently by two agents and by my own searching:

- LIEF *parses* `DyldChainedFixups`, `DyldExportsTrie` and `DyldInfo`, but `Builder.hpp`
  exposes three parallel `build(DyldInfo&)`, `build(DyldChainedFixups&)`,
  `build(DyldExportsTrie&)` methods and **no cross-conversion** — it writes back whichever
  format the input already carried. (verified)
- The Binary Ninja plugins (`xpcmdshell/bn-chained-fixups`, `bb010g/binaryninja-chained-fixups`)
  and Rizin's work apply chains for *analysis*; they do not emit a loadable binary. (verified)
- `dyld_info` has `-fixup_chains` for *display* only. (verified)
- Modern cctools knows the load commands exist — `libstuff/checkout.c` handles
  `LC_DYLD_CHAINED_FIXUPS` and `LC_DYLD_EXPORTS_TRIE` and validates their `__LINKEDIT`
  placement — but there is zero conversion code: `grep -c CHAINED_FIXUPS` over
  `install_name_tool.c`, `vtool.c` and `seg_hack.c` returns 0 for all three. (verified)
- Every search for "downgrade chained fixups", "convert to LC_DYLD_INFO_ONLY", "unchain
  fixups" returns the same advice: relink with `-no_fixup_chains`. (verified)

Nor did anyone find a second project patching `LC_VERSION_MIN_MACOSX`/`LC_BUILD_VERSION` on
a prebuilt vendor binary to relax its OS gate — only `vtool`, which is shipped as a
build-time tool. **`patch_macho` implements a transform that does not appear to exist
elsewhere in public open source.** That is the load-bearing justification for this repo.

### Candidate-by-candidate verdicts

**`tpoechtrager/cctools-port` — partially replaces, and should be adopted rather than
resisted.** Verdict: **replaces `add_version_min` outright; replaces most of `fix_macho`;
replaces `change_dylib`'s `-change`/`-id`/`-rpath`/`-add_rpath`/`-delete_rpath`,
conditionally.**
- `vtool -set-version-min <platform> <minos> <sdk>` and `-remove-build-version` cover
  `add_version_min` and `fix_macho`'s build-version stripping, in place, with no
  `__LINKEDIT` ordering gate. (verified from source)
- `install_name_tool` covers install-name and rpath rewriting on chained-fixups binaries,
  because its `checkout.c` recognises the modern load commands. **Conditional**, because
  `checkout.c` still enforces a long list of other ordering invariants (local relocs, split
  info, function starts, data-in-code, code-signing DRs, symbol/string/indirect/toc/module
  tables, and "link edit information does not fill the `__LINKEDIT` segment"), any of which
  a modern linker may violate. Whether real Electron/Zoom binaries pass is **untested**.
- It does **not** cover: adding, deleting, inserting or reordering `LC_LOAD_DYLIB` with
  library-ordinal renumbering; `-strip-lc`; `-grow`; targeted segment rename; the Swift
  class retag; chained-fixups lowering.
- `seg_hack` is *not* a `rename_segment` replacement: its own header comment says it
  "changes all segments names to the one specified on the command line". Wrong shape.
  (verified)

**`tpoechtrager/osxcross` — does not replace.** Its README states "Host OSes: Linux, \*BSD".
It vendors the same cctools-port code, so it is useful only as a source of SDK-handling
technique. (verified)

**`iains/darwin-xtools` — does not replace, but is the best proof-of-concept for the
packaging work.** It is the only project I found that explicitly modifies cctools/ld64 "to
enable support for older systems with up-to-date-tools" and names Darwin 13/14 as build
hosts, needing only cmake 3.4.1 and C++11. Its ceiling (cctools-906/ld64-351.8, 2018) is
years before chained fixups, so it cannot help with the transform — but it is
independent corroboration that the native-on-Mavericks build path is real. (verified)

**`alexzielenski/optool` — does not replace.** Inserts/removes `LC_LOAD_DYLIB` and strips
signatures. No ordinal renumbering, no fixups, no header growth. Last activity 2019.

**`Tyilo/insert_dylib` — does not replace, but is the closest architectural analog.** It
appends one load command into existing pad and fails rather than reflowing when the pad is
too small — the same discipline, applied to one operation. No fixups logic. Its signature
handling *truncates* `__LINKEDIT`, a size change this project would not accept.

**`lief-project/LIEF` — does not replace; disqualified twice over.** It rebuilds the whole
binary by design (its tutorial says the builder "regenerates and optimizes the rebase
bytecode"), and needs modern C++ and CMake that 2013 clang will not provide. Its value here
is as a *reference implementation of the two bytecode formats* — which is exactly how
`PROVENANCE.md` already credits it. That relationship is correct and should not change.

**`jtool`/`jtool2` — does not replace.** Closed source, deprecated by its author in favour
of DisARM 2, and an inspector rather than a rewriter. A closed binary cannot be rebuilt
for 10.9.

**MachOView — does not replace.** Viewer. Xcode 13+, 10.13+ deployment target, frozen.

**`ldid` / Procursus — does not replace.** Code signing, not load-command surgery. And
per TN2206, 10.9 does not enforce signatures at execution time (see below), so this is
probably not a gap that needs filling at all.

**OpenCore Legacy Patcher — does not replace, and solved a different problem.** Verified by
reading `sys_patch.py`: `OVERWRITE_SYSTEM_VOLUME` / `MERGE_SYSTEM_VOLUME` /
`REMOVE_SYSTEM_VOLUME` and `install_new_file()`. It swaps files. Its only true binary
surgery is fixed-byte-offset opcode patching in a handful of kexts via OpenCorePkg's
generic patch engine — not load-command editing, not reusable here.

**Retroactive — does not replace.** Its Mach-O work is `insert_dylib`: append a load
command, grow `sizeofcmds`, strip the signature. Opposite direction of travel and opposite
constraint (grows the header rather than fitting the pad).

**`llvm-objcopy` / `llvm-install-name-tool` — does not replace.** Narrower than
`install_name_tool`, LLVM-object-model rebuild architecture, requires an LLVM build. No
fixups support found (docs verified; absence inferred from search).

### Net verdict

`macho-tools` is **not redundant**, but it is **larger than it needs to be**. A defensible
future shape: keep `patch_macho` (unique), keep `change_dylib`'s add/delete/insert/reorder/
renumber and `-strip-lc`/`-grow` (unique), keep `rename_segment` and `retag_swift_classes`
(unique), and *retire* `add_version_min` and most of `fix_macho` in favour of a packaged
`vtool` and `install_name_tool`. That also closes out the "this repo still ships two Mach-O
rewriting binaries" complaint in `README.md`, since `fix_macho` is the C one.

Two cheap experiments would settle it:
1. Run a packaged modern `install_name_tool -change` against a real chained-fixups vendor
   binary (Zoom, an Electron app) and see whether `checkout.c` accepts it.
2. Run `vtool -set-version-min macos 10.9 10.9` against the same binary and diff the result
   against `add_version_min`'s.

---

## Packaging candidates for ModernMavericks

Ranked by (ports unblocked) × (ease of packaging). The family has 14 public repos and none
ships binutils, an SDK, libtapi, or a linker other than lld.

### 1. `mavericks-cctools` — highest value, lowest risk

**Ports unblocked: all of them.** Every port that inspects or edits a Mach-O currently
finds an xcselect shim or nothing at all. `mavericks-clang` ships clang and lld (verified:
`build/build-native.sh` uses `-DLLVM_ENABLE_PROJECTS="clang;lld" … -DCLANG_DEFAULT_LINKER=lld`),
so `otool`, `nm`, `strip`, `lipo`, `ar`, `ranlib`, `libtool`, `size`, `strings`,
`install_name_tool`, `nmedit`, `vtool` and `dyldinfo` are all missing family-wide.
`macho-tools` itself would consume this — its own testing wants a real `otool` and `dyldinfo`.

**Pin the `master` branch, not the default branch.** This matters and is easy to get wrong.
cctools-port's default branch is `1030.6.3-ld64-956.6`, whose README demands "Clang 10+,
libstdc++ or libc++ with C++20 support… libdispatch-dev and libblocksruntime" — not
bootstrappable on 10.9. WebKit's pin `e79d784d667816e4b15a0abd78828f9abb0a0b99` is the tip
of **`master`** (cctools 973.0.1 / ld64 609, README: "Clang 3.4 or later"), dated
2024-09-14. Verified via the branch API. Every source fact in this report — `vtool`'s
in-place `command_set`, `checkout.c`'s chained-fixups awareness, `install_name_tool`'s
headerpad error — was read on `master`, so the lighter branch is also the capable one for
these purposes.

**Ease: unusually high, because the recipe already exists and self-verifies.** Verbatim from
`MavericksSupport/toolchain/scripts/build_cctools.sh`:

- One patch, `cctools-drop-vestigial-blob-clone.patch`: removes `BlobType *clone()` from
  `ld64/src/ld/code-sign-blobs/blob.h`, because "the name is qualified and BlobCore is not a
  dependent type, so the lookup happens when the template is defined rather than when it is
  instantiated, and every C++ translation unit that reaches this header fails."
- Two 10.9 gap-fills, force-loaded as a static archive: `open_memstream` (10.13+, called by
  libstuff's error paths) and `_availability_version_check` (referenced by clang's
  `__builtin_available` lowering). **Both already exist in the family**, in
  `mavericks-macports-legacy-support` — its coverage table lists `open_memstream` at OSX10.12.
- `./configure --disable-tapi-support --disable-lto-support --disable-xar-support`.
- `make` in exactly `libstuff`, `libmacho`, `ar`, `misc`, `otool`, `ld64/src/3rd`, and
  `ld64/src/other dyldinfo`. A whole-tree make is explicitly avoided because it "would also
  build ld, whose libcodedirectory.c calls htonll and DISPATCH_APPLY_AUTO, both 10.10+".
- Only `dyldinfo` is C++, and 10.9 ships no libc++ headers, so it builds against a modern SDK:
  `-std=c++11 -isysroot $SDK -mmacosx-version-min=10.9 -stdlib=libc++`.
- The script ends by running all twelve tools against `/usr/lib/dyld`, `/usr/lib/libz.1.dylib`
  and a freshly compiled object, because "a tool that builds but cannot run is the failure this
  whole change exists to stop". That is a ready-made shipyard smoke test.

**Shape of the work:** an ordinary family repo. `UPSTREAM_VERSION` pinned to the cctools-port
`master` commit with a Renovate customManager against that repo (unlike `macho-tools`, this is
a real external upstream, so `INGREDIENTS.md`'s "no datasource" problem does not recur); the
patch carried in-tree; the two shims taken as a dependency on `mavericks-macports-legacy-support`
rather than re-vendored; the seven `make` targets; the twelve-tool probe as the ctest; the
shipyard compat guard confirming each binary declares a 10.9 floor. `misc/` also builds `vtool`
and `seg_hack`, which WebKit compiles but does not install — install them.

**Caveat, honestly stated:** WebKit builds this *on a 10.9 host with clang-22*. Whether it
cross-builds cleanly on the family's arm64 CI runners the way `mavericks-clang` does is
**unverified** — the autotools `configure` may need cross plumbing the native path never
exercised. Budget for that being the actual work.

### 2. `mavericks-sdk` — high value, medium effort

WebKit builds against `MacOSX26.1.sdk` with `-mmacosx-version-min=10.9` and carries a
`patch-sdk.sh` for "the two edits the build needs in the macOS SDK". `mavericks-shipyard`
already pins a MacOSX10.9 SDK per `INGREDIENTS.md`, so the family has *an* SDK story — but the
"modern SDK, ancient deployment target" pattern, which is what lets C++ code with libc++ headers
build for 10.9, is currently solved per-port. `phracker/MacOSX-SDKs` is stale (last push
2022-11-11), so the family would be sourcing its own; `alexey-lysiuk/macos-sdk` (push 2026-07-02)
and `devernay/xcodelegacy` (push 2026-07-23) are the maintained alternatives worth evaluating.
Effort is mostly licensing and distribution judgement rather than engineering.

### 3. Build-host tools: cmake, ninja, python3, ruby, nasm, git

Verified from the WebKit README that all of these are built into `toolchain/build/` on the 10.9
host. This is pure duplicated toil — every port using CMake or Meson needs a modern CMake and
Ninja on 10.9, and each one solves it alone. Lower per-item value than cctools, very high in
aggregate, and each is individually easy. One repo per tool fits the family convention better
than a bundle.

### 4. `mavericks-polyfill` — the framework-level shims

`mavericks-macports-legacy-support` covers the libc gaps. WebKit's `polyfill/` goes further:
"the C functions and data constants: the libc gap-fills plus the framework entry points 10.9
lacks or gets wrong", plus Objective-C method and class polyfills. The Objective-C half is very
WebKit-specific; the framework-entry-point half is likely general. Effort: high, because
separating the general part from the WebKit-specific part requires judgement this survey does
not have.

### 5. `mavericks-ld64` — speculative, do not start here

`ld` from cctools-port `master` does **not** build on 10.9 (verified: `htonll` and
`DISPATCH_APPLY_AUTO` in `libcodedirectory.c` are 10.10+), and the newer branch that *is*
chained-fixups-capable needs C++20. Since `mavericks-clang` already defaults to lld, there may
be no need at all. Note that nobody else has closed this gap either: MacPorts defaults Darwin 13
to ld64-274, and darwin-xtools stops at ld64-351.8. Revisit only if a port turns up that lld
cannot link.

### Not recommended

- **Packaging `LIEF`** — modern C++/CMake, rebuild-the-file architecture. Keep it as a format
  reference, as `PROVENANCE.md` already does.
- **Packaging `ldid` or `rcodesign`** — per TN2206, 10.9 does not enforce signatures at
  execution; signing matters only for Gatekeeper on *downloaded* apps. `rcodesign` cannot run
  on 10.9 regardless (Rust's floor is 10.12 since 1.74).
- **Anything from Procursus** — macOS floor is Big Sur.
- **`apple-libtapi`** — WebKit sidesteps it with `--disable-tapi-support`; no evidence it is
  needed, and it would drag in an LLVM build.

### Standing risk worth a separate look: LLVM's rising libc++ floor

Verified on LLVM Discourse: libc++'s minimum supported macOS deployment target rises from
**10.13 to 11.0 in LLVM v22.1**, driven by an unconditional dependency on `aligned_alloc`
(macOS 11+), with maintainer policy framed as supporting "what is known to be used and to
deliver value". `mavericks-clang`'s current release notes file is `22.1.1-mavericks.1.md`, and
its build tree already carries a `build/shim/aligned_alloc.h` — so the family may already be
working around exactly this. Whether it fully bites (mavericks-clang builds with
`LLVM_ENABLE_RUNTIMES=""` and sources runtime headers separately) is **unverified and outside
this survey's scope**, but it is the kind of upstream drift that ends a project quietly, and it
deserves its own investigation.

---

## What I could not determine

- **Whether a modern `install_name_tool` actually accepts real vendor binaries.** I verified
  that `checkout.c` recognises `LC_DYLD_CHAINED_FIXUPS` and `LC_DYLD_EXPORTS_TRIE`, removing
  the *specific* reason 10.9's tool refuses. I did **not** verify that Zoom's or an Electron
  app's `__LINKEDIT` satisfies the dozen other ordering invariants in the same function. This
  is the highest-value unknown in the survey and it is cheaply testable.
- **Whether cctools-port cross-builds** on the family's modern arm64 CI runners. The only
  recipe verified is a native 10.9 build.
- **Whether an *invalidated* code signature blocks loading on 10.9.** TN2206 establishes that
  10.9 does not require signatures for execution (Gatekeeper handles quarantined downloads
  only), but I did not confirm what 10.9's dyld does with a present-but-now-wrong
  `LC_CODE_SIGNATURE` after an edit. If it rejects, `-strip-lc` on the signature becomes
  mandatory rather than optional.
- **`apple-libtapi`'s C++ requirements and 10.9 buildability.** WebKit sidesteps it, so there
  is no evidence either way.
- **Procursus's stated macOS bootstrap floor.** `docs.procurs.us` returned HTTP 402. Big Sur is
  inferred from Homebrew bottle coverage (Ventura/Sonoma+) and the MacPorts port's clang-18
  build dependency, both of which were verified.
- **`opa334/ChOma`** — a dependency-light C Mach-O and code-signature library from the
  TrollStore/Dopamine world. Surfaced but not inspected. It is the right *shape* for this
  problem, so it is worth a look before declaring the search exhausted, though nothing
  suggests it does fixups lowering.
- **A clean quotable MacPorts "oldest supported OS" statement.** Repeated attempts failed; the
  Portfile evidence above is a proxy, not a policy statement.
- **`Wowfunhappy/Mavericks-Porting-Resources` beyond the extracted tools.** Its README warns
  "This repository consists almost entirely of AI generated code that a human has not
  reviewed." Not audited for other things the family might want.
- **`optool`'s exact last commit.** The API reports `pushed_at` 2019-09-06; a subagent reported
  a master commit of 2017-03-30. Either way it is abandoned.
