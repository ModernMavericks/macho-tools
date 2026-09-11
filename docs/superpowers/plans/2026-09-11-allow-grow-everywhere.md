# allow-grow Everywhere Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every operation that can run out of header pad honours `allow-grow` on the images growth supports, through one function that decides whether there is room, and every refusal names the real remedy.

**Architecture:** A new `mg_ensure_pad` in `src/grow.c` owns "does the load-command region fit, and may we grow?". `mr_process_thin`'s inline pad block becomes a call to it; `mv_add_version_min_image` becomes its second caller, taking a reallocatable buffer. `edit`'s `version-min set` and a new `macho9 minos --allow-grow` pass the permission through. Grow's own refusals stop naming `change_dylib`, `patch_macho`, and a document that does not exist.

**Tech Stack:** C99, stock 10.9 AppleClang 6.0, CMake + ctest. Hermetic C tests in `tests/grow_test.c`; CLI behaviour in `tests/cli_test.sh`.

**Spec:** `docs/superpowers/specs/2026-09-11-allow-grow-everywhere-design.md`

## Global Constraints

- **`tests/EXPECTED` is never edited.** `sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check` must print `characterize: OK (ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792)`.
- **The six `compat/` wrappers' stdout stays byte-identical.** `tests/known-callers.sh` and `tests/wrapper_test.sh` (both under ctest) are the gates. The grow path's stdout lines — "load commands need N more bytes than the M-byte pad; growing header..." and "grew header pad: first sect now at N (M bytes available)" — are on `change_dylib -grow`'s path and `tests/change_dylib_test.sh:1794-1810` counts them. Their text does not change.
- **Growth only on 64-bit `MH_EXECUTE` with `MH_PIE`,** and never on an image still carrying `LC_DYLD_CHAINED_FIXUPS`. Those checks stay in `mg_grow_header`/`mg_classify`; nothing here duplicates them.
- **`fixups set classic` does not honour `allow-grow`.** Out of scope by design.
- **Exit codes:** 0 ok, 1 refused (MR_REFUSED / EX_REFUSED), 2 error (MR_FAIL / EX_FAIL). A no-room refusal is 1.
- **The tools never move a byte of file data,** except the two reviewed exceptions: grow's memmove after the load commands, and the export-trie append past `__LINKEDIT`'s end.
- **POSIX `/bin/sh` only** in test scripts. No `[[`, `local`, `+=`, arrays, `<<<`, `$'...'`, `function`, `source`, `shopt`.
- **Warning-free** under stock 10.9 AppleClang 6.0.
- **A comment or doc that claims more than the code does is a defect.** Committed text never names plan artifacts ("Task N", "Ruling N", "the spec says").
- **Stage explicit paths only.** Never `git add -A`, `git add .`, or `git commit -a`.

Build dir for every command below: `B=/private/tmp/mm-build/schmonz/macho-tools/native` (already configured).

---

### Task 1: `mg_ensure_pad`, and grow's refusals name the real remedy

**Files:**
- Modify: `src/grow.h` — declare `mg_ensure_pad`
- Modify: `src/grow.c` — define it; rewrite four refusal messages (`:583`, `:612-613`, `:919-922`, `:926-928`)
- Modify: `tests/grow_test.c` — a `MG_T_CHAINED` builder option and five tests

**Interfaces:**
- Consumes: `mg_first_sect_off(const uint8_t *buf, size_t fsize)` and `mg_grow_header(uint8_t **pbuf, size_t *pfsize, uint32_t grow_req)`, both existing in `src/grow.c`.
- Produces:

```c
int mg_ensure_pad(uint8_t **pbuf, size_t *pfsize, uint32_t need_end,
                  int allow_grow, const char *label);
```

Returns 0 if the load-command region already reaches `need_end` without crossing the first section (image untouched, buffer not reallocated), or 0 after growing it. Returns -1 when it does not fit and growth was not permitted or failed. Tasks 2 and 3 call it.

- [ ] **Step 1: Add the chained-fixups builder option to `tests/grow_test.c`**

Beside the other `MG_T_*` defines (after `MG_T_ATOM_INFO`, around `:233`):

```c
#define MG_T_CHAINED 2048    /* LC_DYLD_CHAINED_FIXUPS: refused by mg_classify,
                               * because chained pointers encode offsets from the
                               * image base, which growing moves. See
                               * test_ensure_pad_refuses_what_cannot_grow. */
```

In `build_image`, directly after the `MG_T_ATOM_INFO` block (which adds a `linkedit_data_command` the same way):

```c
    if (opts & MG_T_CHAINED) {
        struct linkedit_data_command *cf = (struct linkedit_data_command *)lcend;
        cf->cmd = LC_DYLD_CHAINED_FIXUPS;
        cf->cmdsize = sizeof *cf;
        cf->dataoff = 6656; cf->datasize = 8;
        h->ncmds++; h->sizeofcmds += cf->cmdsize; lcend += cf->cmdsize;
    }
```

- [ ] **Step 2: Write the failing tests**

Add after `test_grow_refuses_32bit_mach_header` (which defines the `stderr_contains_during` helper these reuse). That helper takes a `(uint8_t **, size_t *, uint32_t)` callee, so a thunk carries `mg_ensure_pad`'s other arguments:

```c
/* ---- mg_ensure_pad: the one place that decides whether there is room ---- */

static uint32_t g_ensure_need;
static int      g_ensure_allow;
static int ensure_thunk(uint8_t **pbuf, size_t *pfsize, uint32_t unused) {
    (void)unused;
    return mg_ensure_pad(pbuf, pfsize, g_ensure_need, g_ensure_allow, "t");
}

static void test_ensure_pad_fits_is_a_noop(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_growable_image(&fsize, &sect_off);
    uint8_t *orig = buf;
    size_t fsize0 = fsize;
    uint8_t *before = (uint8_t *)malloc(fsize0);
    memcpy(before, buf, fsize0);

    const struct mach_header_64 *h = (const struct mach_header_64 *)buf;
    uint32_t lc_end = (uint32_t)sizeof *h + h->sizeofcmds;
    int r = mg_ensure_pad(&buf, &fsize, lc_end, 0, "t");
    CHECK(r == 0, "ensure_pad: the current load commands fit (got %d)", r);
    r = mg_ensure_pad(&buf, &fsize, sect_off, 0, "t");
    CHECK(r == 0, "ensure_pad: reaching exactly the first section still fits (got %d)", r);
    CHECK(buf == orig && fsize == fsize0, "ensure_pad: a fit neither reallocates nor resizes");
    CHECK(memcmp(before, buf, fsize0) == 0, "ensure_pad: a fit leaves every byte alone");
    free(before);
    free(buf);
}

static void test_ensure_pad_short_and_not_permitted_refuses(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_growable_image(&fsize, &sect_off);
    size_t fsize0 = fsize;
    uint8_t *before = (uint8_t *)malloc(fsize0);
    memcpy(before, buf, fsize0);

    g_ensure_need = sect_off + 1; g_ensure_allow = 0;
    int r;
    int said = stderr_contains_during(ensure_thunk, &buf, &fsize, 0,
                                      "growing the header needs allow-grow", &r);
    CHECK(r == -1, "ensure_pad: short and not permitted is refused (got %d)", r);
    CHECK(said, "ensure_pad: the refusal names allow-grow as the remedy");
    CHECK(fsize == fsize0 && memcmp(before, buf, fsize0) == 0,
          "ensure_pad: a refusal leaves the image byte-identical");
    free(before);
    free(buf);
}

static void test_ensure_pad_grows_when_permitted(void) {
    size_t fsize; uint32_t sect_off;
    /* MG_T_FUNCSTARTS so the plausibility check below has function starts
     * and initializers to check against each other after the base moved. */
    uint8_t *buf = build_image(&fsize, &sect_off, MG_T_FUNCSTARTS);
    size_t fsize0 = fsize;
    int r = mg_ensure_pad(&buf, &fsize, sect_off + 1, 1, "t");
    CHECK(r == 0, "ensure_pad: short and permitted grows (got %d)", r);
    CHECK(fsize >= fsize0 + MG_PAGE, "ensure_pad: the image grew by at least a page "
          "(got %zu, was %zu)", fsize, fsize0);
    CHECK(mg_first_sect_off(buf, fsize) == sect_off + MG_PAGE,
          "ensure_pad: the first section moved out by one page (got %u, was %u)",
          mg_first_sect_off(buf, fsize), sect_off);
    CHECK(mg_plausible(buf, fsize) == 0, "ensure_pad: the grown image is plausible");
    free(buf);
}

static void check_ensure_refuses_unchanged(const char *what, int opts,
                                           uint32_t filetype, uint32_t flags,
                                           const char *needle) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_image(&fsize, &sect_off, opts);
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->filetype = filetype;
    h->flags = flags;
    size_t fsize0 = fsize;
    uint8_t *before = (uint8_t *)malloc(fsize0);
    memcpy(before, buf, fsize0);

    g_ensure_need = sect_off + 1; g_ensure_allow = 1;
    int r;
    int said = stderr_contains_during(ensure_thunk, &buf, &fsize, 0, needle, &r);
    CHECK(r == -1, "ensure_pad on %s: refused even when permitted (got %d)", what, r);
    CHECK(said, "ensure_pad on %s: the refusal says '%s'", what, needle);
    CHECK(fsize == fsize0 && memcmp(before, buf, fsize0) == 0,
          "ensure_pad on %s: the image is byte-identical", what);
    free(before);
    free(buf);
}

static void test_ensure_pad_refuses_what_cannot_grow(void) {
    check_ensure_refuses_unchanged("a dylib", 0, MH_DYLIB, MH_PIE,
                                   "cannot grow a dylib or bundle");
    check_ensure_refuses_unchanged("a non-PIE executable", 0, MH_EXECUTE, 0,
                                   "not PIE");
    check_ensure_refuses_unchanged("an image with chained fixups", MG_T_CHAINED,
                                   MH_EXECUTE, MH_PIE, "fixups set classic");
}
```

Add all four `test_ensure_pad_*` calls at the end of `main()`, before its final summary line.

- [ ] **Step 3: Run and watch it fail to build**

```bash
cmake --build $B 2>&1 | grep -m3 "mg_ensure_pad"
```

Expected: an implicit-declaration or undefined-symbol error for `mg_ensure_pad`.

- [ ] **Step 4: Declare `mg_ensure_pad` in `src/grow.h`**

After the `mg_first_sect_off` declaration:

```c
/* Ensure the load commands can extend to `need_end` bytes from the start of
 * the image -- sizeof(struct mach_header_64) plus the sizeofcmds the caller is
 * about to write -- without crossing the first section's data. This is the
 * one place that decides whether there is room and whether to grow.
 *
 * Returns 0 with the image untouched (not reallocated) if it already fits.
 * Otherwise, when `allow_grow` is set, grows the header pad through
 * mg_grow_header and returns 0 with *pbuf/*pfsize updated: every pointer the
 * caller held into the buffer is stale. Prints, on stdout, the two lines the
 * grow path has always printed ("load commands need ...; growing header...",
 * "grew header pad: ...").
 *
 * Returns -1, with the reason on stderr prefixed by `label`, when it does not
 * fit and growth was not permitted, or when growth failed. If growth was
 * refused on a precondition (not a PIE executable, chained fixups, a load
 * command whose payload grow cannot re-base) the image is untouched; a
 * failure partway through growing can leave it modified. Either way the
 * caller must not write it, and *pbuf stays valid to free. */
int mg_ensure_pad(uint8_t **pbuf, size_t *pfsize, uint32_t need_end,
                  int allow_grow, const char *label);
```

- [ ] **Step 5: Define it in `src/grow.c`**

After `mg_first_sect_off`'s definition:

```c
int mg_ensure_pad(uint8_t **pbuf, size_t *pfsize, uint32_t need_end,
                  int allow_grow, const char *label) {
    uint32_t first = mg_first_sect_off(*pbuf, *pfsize);
    if (first == UINT32_MAX) {
        fprintf(stderr, "ERROR: %s fails validation; refusing (see above)\n", label);
        return -1;
    }
    if (need_end <= first) return 0;

    const struct mach_header_64 *hdr = (const struct mach_header_64 *)*pbuf;
    uint32_t cur_lc_end = (uint32_t)sizeof *hdr + hdr->sizeofcmds;
    uint32_t pad_avail  = first > cur_lc_end ? first - cur_lc_end : 0;
    uint32_t new_lcs    = need_end - (uint32_t)sizeof *hdr;
    if (!allow_grow) {
        fprintf(stderr, "ERROR: %s: new LCs (%u bytes) don't fit in header pad (%u avail); "
                        "growing the header needs allow-grow\n", label, new_lcs, pad_avail);
        return -1;
    }

    uint32_t grow_req = need_end - first;
    printf("%s: load commands need %u more bytes than the %u-byte pad; growing header...\n",
           label, grow_req, pad_avail);
    if (mg_grow_header(pbuf, pfsize, grow_req) != 0) {
        fprintf(stderr, "ERROR: %s: new LCs (%u bytes) don't fit and header could not be grown\n",
                label, new_lcs);
        return -1;
    }
    first = mg_first_sect_off(*pbuf, *pfsize);
    if (first == UINT32_MAX) {
        fprintf(stderr, "ERROR: %s: header grow produced an image that fails validation\n", label);
        return -1;
    }
    printf("%s: grew header pad: first sect now at %u (%u bytes available)\n",
           label, first, first - cur_lc_end);
    return 0;
}
```

The "allow-grow" wording is deliberate: it is the name the `edit` directive and `macho9`'s `--allow-grow` share, and `compat/change_dylib.sh:32` documents its `-grow` as `--allow-grow`.

- [ ] **Step 6: Rewrite grow's four refusal messages**

In `src/grow.c`:

1. `mg_classify`'s chained-fixups reason (`:583-584`):
   ```c
            why = "LC_DYLD_CHAINED_FIXUPS: chained pointers encode offsets from the "
                  "image base, which growing moves; convert them first (`fixups set "
                  "classic` in an edit script, or `macho9 declassify`)";
   ```
2. `mg_classify`'s shared suffix (`:612-613`), which names `change_dylib`'s flags:
   ```c
            fprintf(stderr, "macho_grow: %s. Refusing to grow. Reclaim header bytes "
                            "instead by deleting load commands (uuid, codesig).\n", why);
   ```
3. `mg_grow_header`'s filetype refusal (`:919-922`):
   ```c
        fprintf(stderr, "macho_grow: only MH_EXECUTE can be grown (filetype=%u): growing "
                        "lowers the image base into __PAGEZERO, and a dylib or bundle has "
                        "none. This tool cannot grow a dylib or bundle.\n", hdr->filetype);
   ```
4. `mg_grow_header`'s PIE refusal (`:926-928`):
   ```c
        fprintf(stderr, "macho_grow: executable is not PIE (flags=0x%x); lowering the "
                        "image base would require fixing absolute relocations, which "
                        "this tool does not do\n", hdr->flags);
   ```

Then read the comment block above `mg_grow_header`'s precondition checks (`:888-913`) and the file header of `src/grow.h`: remove any reference to `HEADER_PAD_GROWTH.md`, which is not in this repository, without changing what they otherwise say.

- [ ] **Step 7: Run the tests and watch them pass**

```bash
cmake --build $B && ctest --test-dir $B -R grow_test --output-on-failure
```

Expected: `grow_test` passes, and the build has no warnings.

- [ ] **Step 8: Prove the tests can fail**

Two mutations, one at a time, each reverted before the next:

1. Temporarily delete the `case LC_DYLD_CHAINED_FIXUPS:` block from `mg_classify` (the command then falls through to the unclassified-command refusal, so `r` stays -1), rebuild, and confirm `ensure_pad on an image with chained fixups: the refusal says 'fixups set classic'` fails.
2. Temporarily make `mg_ensure_pad` ignore its permission (change `if (!allow_grow)` to `if (0)`), rebuild, and confirm `ensure_pad: short and not permitted is refused` fails.

Record both mutations and their output in the task report.

- [ ] **Step 9: Run the whole suite and commit**

```bash
ctest --test-dir $B
sh tests/characterize.sh $B check
git add src/grow.h src/grow.c tests/grow_test.c
git commit -m "feat: mg_ensure_pad decides whether load commands fit; grow names real remedies"
```

---

### Task 2: `mr_process_thin` asks `mg_ensure_pad`

**Files:**
- Modify: `src/rewrite.c:778-799` — the inline pad block

**Interfaces:**
- Consumes: `mg_ensure_pad` from Task 1.
- Produces: no new interface. `dylib`, `rpath`, `lc`, `segment` and every wrapper behave exactly as before, except the stderr line when growth is not permitted.

- [ ] **Step 1: Record the base and build the comparison binaries**

```bash
BASE=$(git rev-parse HEAD)
S=${TMPDIR:-/tmp}/aged.$$; mkdir -p "$S"
git worktree add "$S/old" "$BASE"
cmake -S "$S/old" -B "$S/old-build" >/dev/null && cmake --build "$S/old-build" >/dev/null
```

The worktree is scratch: remove it with `git worktree remove --force "$S/old"` when this task is done.

- [ ] **Step 2: Replace the inline block**

In `mr_process_thin`, replace from `if (need_end > first_sect_off) {` through the `printf("%s: grew header pad: ...` statement (inclusive) with:

```c
    if (need_end > first_sect_off) {
        /* Whether there is room, and whether to grow, is mg_ensure_pad's
         * decision (src/grow.h) -- one place, shared with version-min. It
         * prints the grow path's stdout lines itself, unchanged. */
        if (mg_ensure_pad(&buf, &fsize, need_end, ops->allow_grow, label) != 0) {
            *pbuf = buf; *pfsize = fsize;   /* growth may have realloc'd before failing */
            free(new_lcs);
            return MR_ERROR;
        }
        *pbuf = buf; *pfsize = fsize;       /* mg_ensure_pad may have realloc'd */
        hdr = (struct mach_header_64 *)buf;
        first_sect_off = mg_first_sect_off(buf, fsize);
        if (first_sect_off == UINT32_MAX) {
            fprintf(stderr, "ERROR: %s: header grow produced an image that fails validation\n", label);
            free(new_lcs);
            return MR_ERROR;
        }
```

Everything after — the comment beginning "mg_grow_header reallocs the raw buffer", the `mi_wrap` re-wrap, and the rebuild of `new_lcs` — stays exactly as it is. The re-wrap comment still names `mg_grow_header`; it is still the function that reallocated, now through `mg_ensure_pad`, so adjust its first line to say "growth (mg_ensure_pad -> mg_grow_header) reallocs the raw buffer".

`pad_avail` and `cur_lc_end` are still used by the "header pad N bytes available" line near the top of the function, so neither becomes unused.

- [ ] **Step 3: Run the suite**

```bash
cmake --build $B && ctest --test-dir $B && sh tests/characterize.sh $B check
```

Expected: all pass (chained_fixups skips on 10.9), characterize OK, no warnings.

- [ ] **Step 4: Compare old and new on the grow paths**

```bash
long="@loader_path/$(printf '%3000s' '' | tr ' ' y).dylib"
cmp_run() {   # cmp_run NAME ARGS... (FILE is the literal word f)
    name=$1; shift
    for side in old new; do
        if [ $side = old ]; then d="$S/old-build"; else d="$B"; fi
        cp tests/fixture.macho "$S/f"
        ( cd "$S" && "$d/$@" ) >"$S/$side.out" 2>"$S/$side.err"; echo $? >"$S/$side.rc"
        shasum -a 256 <"$S/f" >"$S/$side.sha"
    done
    for k in out rc sha; do
        cmp -s "$S/old.$k" "$S/new.$k" || echo "DIFF $name: $k"
    done
    cmp -s "$S/old.err" "$S/new.err" || echo "stderr differs $name (expected only for a not-permitted refusal)"
}
cmp_run dylib-append-nogrow  macho9 dylib f -append "$long"
cmp_run dylib-append-grow    macho9 dylib f --allow-grow -append "$long"
cmp_run rpath-append-grow    macho9 rpath f --allow-grow -append "$long"
cmp_run dylib-append-short   macho9 dylib f -append @loader_path/x.dylib
cmp_run cd-add-nogrow        change_dylib f -add "$long"
cmp_run cd-add-grow          change_dylib f -grow -add "$long"
cmp_run cd-add-rpath-grow    change_dylib f -grow -add-rpath "$long"
```

Expected: no `DIFF` line at all. The only `stderr differs` lines are `dylib-append-nogrow` and `cd-add-nogrow`, and in each the only changed line is the no-room refusal ("pass -grow to enlarge it" became "growing the header needs allow-grow"). Show the output in the task report. If `change_dylib` rejects `-add`/`-add-rpath` on this fixture, use the spelling `compat/change_dylib.sh`'s header documents and say so.

- [ ] **Step 5: Commit**

```bash
git worktree remove --force "$S/old"
git add src/rewrite.c
git commit -m "refactor: mr_process_thin asks mg_ensure_pad whether the load commands fit"
```

---

### Task 3: `version-min set` grows under `allow-grow`

**Files:**
- Modify: `src/version_min.h`, `src/version_min.c` — the in-memory signature, the room check, the file path
- Modify: `src/edit.c` — the MS_VERSION_MIN arm
- Modify: `src/edit.h` — the DIRECTIVES paragraph (`:146-153`)
- Modify: `cli/macho9.c:588` — `cmd_minos` passes 0 for now (Task 4 adds the flag)
- Modify: `tests/cli_test.sh` — the tight fixture and the `edit` cases

**Interfaces:**
- Consumes: `mg_ensure_pad` from Task 1.
- Produces:

```c
int mv_add_version_min_image(uint8_t **pbuf, size_t *psize, int allow_grow,
                             int *out_added);
int mv_add_version_min(const char *path, int allow_grow);
```

Task 4 calls `mv_add_version_min(path, allow_grow)` from `cmd_minos`.

- [ ] **Step 1: Write the failing test**

At the end of `tests/cli_test.sh`, before `reached_end=1`:

```sh
# version-min set and allow-grow. LC_VERSION_MIN_MACOSX needs 16 bytes of
# header pad, and build_main's pad is far larger, so a fixture that is
# genuinely short has to be made: strip any LC_VERSION_MIN_MACOSX the
# linker emitted (strip_version_min, above), then fill the pad with a dylib
# append whose LC_LOAD_DYLIB is the largest multiple of 8 that fits. An
# LC_LOAD_DYLIB is 24 bytes plus the path and its NUL, rounded up to 8, so a
# path of C-25 bytes makes a command of exactly C, leaving pad % 8 bytes --
# fewer than 16. Sized from `macho9 info`, not hard-coded, because each
# host's linker leaves a different pad.
vm_pad_of() {
    "$MACHO9" info "$1" | sed -n 's/^header pad: \([0-9][0-9]*\) bytes available.*/\1/p'
}
build_main "$T/vm_tight"
"$T/strip_version_min" "$T/vm_tight" >/dev/null \
    || bad "version-min allow-grow: fixture setup" "strip_version_min failed"
vm_pad=$(vm_pad_of "$T/vm_tight")
if [ -z "$vm_pad" ] || [ "$vm_pad" -lt 32 ]; then
    bad "version-min allow-grow: fixture setup" "pad '$vm_pad' too small to size a filler"
    vm_pad=32
fi
vm_cmd=$((vm_pad - vm_pad % 8))
vm_fill="/$(printf "%$((vm_cmd - 26))s" '' | tr ' ' v)"
"$MACHO9" dylib "$T/vm_tight" -append "$vm_fill" >/dev/null 2>"$T/vm_fill.err" \
    || bad "version-min allow-grow: fixture setup" "filler append failed: $(cut -c1-160 "$T/vm_fill.err")"
vm_left=$(vm_pad_of "$T/vm_tight")
[ -n "$vm_left" ] && [ "$vm_left" -lt 16 ] \
    && ok "version-min allow-grow: fixture has ${vm_left} bytes of pad, fewer than the 16 needed" \
    || bad "version-min allow-grow: fixture setup" "expected fewer than 16 bytes of pad, got '$vm_left'"

cp "$T/vm_tight" "$T/vm_e"
vm_before=$(sha "$T/vm_e"); vm_ino=$(stat -f %i "$T/vm_e")
printf 'version-min set 10.9\n' >"$T/vm_no.edits"
printf 'allow-grow\nversion-min set 10.9\n' >"$T/vm_yes.edits"
rc=0
"$MACHO9" edit "$T/vm_e" "$T/vm_no.edits" >/dev/null 2>"$T/vm_no.err" || rc=$?
[ "$rc" -eq 1 ] && ok "edit: version-min set without allow-grow is refused (1) when the pad is short" \
    || bad "edit version-min" "without the directive: expected 1, got $rc: $(cat "$T/vm_no.err")"
[ "$(sha "$T/vm_e")" = "$vm_before" ] && [ "$(stat -f %i "$T/vm_e")" = "$vm_ino" ] \
    && ok "edit: ... and the refused run left the file unchanged" \
    || bad "edit version-min" "the refused run modified the file"
grep -q "growing the header needs allow-grow" "$T/vm_no.err" \
    && ok "edit: ... and the refusal names allow-grow as the remedy" \
    || bad "edit version-min" "no allow-grow remedy in: $(cat "$T/vm_no.err")"
rc=0
"$MACHO9" edit "$T/vm_e" "$T/vm_yes.edits" >/dev/null 2>"$T/vm_yes.err" || rc=$?
[ "$rc" -eq 0 ] && ok "edit: version-min set with allow-grow grows the header and succeeds" \
    || bad "edit version-min" "with the directive: expected 0, got $rc: $(cat "$T/vm_yes.err")"
"$MACHO9" info "$T/vm_e" | grep -q "LC_VERSION_MIN_MACOSX" \
    && ok "edit: allow-grow: LC_VERSION_MIN_MACOSX is in the written image" \
    || bad "edit version-min" "no LC_VERSION_MIN_MACOSX after the grow"
"$MACHO9" verify "$T/vm_e" >/dev/null 2>"$T/vm_verify.err" \
    && ok "edit: allow-grow: the grown image passes macho9 verify" \
    || bad "edit version-min" "verify refused: $(cat "$T/vm_verify.err")"
```

- [ ] **Step 2: Run and watch it fail**

```bash
sh tests/cli_test.sh $B 2>&1 | grep "version-min"
```

Expected: the fixture lines pass; `version-min set with allow-grow grows the header and succeeds` FAILs with exit 1, and "no LC_VERSION_MIN_MACOSX after the grow" FAILs. (The not-permitted refusal may or may not already contain "allow-grow" text; the with-directive case is the one that must fail.)

- [ ] **Step 3: Change `src/version_min.h`**

Replace both declarations and their comments:

```c
/*
 * Append LC_VERSION_MIN_MACOSX 10.9 to the thin 64-bit Mach-O at `path`,
 * writing the result back in place. Returns 0 on success -- including the
 * "already has one, nothing to do" case -- or MR_REFUSED/MR_FAIL with a
 * message already printed on stderr.
 *
 * The new command goes in the header pad. If the pad cannot hold it, this
 * refuses -- unless `allow_grow` is set and the image can be grown (a 64-bit
 * PIE executable without chained fixups; see mg_ensure_pad in src/grow.h),
 * in which case the pad is enlarged and the file written back grows with it.
 * Fat containers are not handled: add_version_min never did.
 */
int mv_add_version_min(const char *path, int allow_grow);

/*
 * mv_add_version_min's edit, without the file: append LC_VERSION_MIN_MACOSX
 * 10.9 to the image in *pbuf, and nothing else -- no open, no race guard, no
 * write. src/edit.c calls it for `version-min set 10.9` against the image it
 * writes once, itself, after the last statement.
 *
 * Returns 0 with *out_added = 1 if it appended the command, 0 with
 * *out_added = 0 if the image already had one (after printing "already
 * present; nothing to do." on stdout, as mv_add_version_min always has), or
 * MR_REFUSED with "no room for LC_VERSION_MIN_MACOSX" on stderr when the
 * command cannot be placed. When the pad is short and `allow_grow` is set,
 * growing it is mg_ensure_pad's decision; if it grows, *pbuf is reallocated,
 * *psize is larger, and every pointer the caller held into the buffer is
 * stale.
 */
int mv_add_version_min_image(uint8_t **pbuf, size_t *psize, int allow_grow,
                             int *out_added);
```

Add `#include <stddef.h>` and `#include <stdint.h>` to the header if they are not already reached through `image.h`.

- [ ] **Step 4: Change `mv_add_version_min_image` in `src/version_min.c`**

Add `#include "grow.h"` beside the other includes. Replace the function body:

```c
int mv_add_version_min_image(uint8_t **pbuf, size_t *psize, int allow_grow,
                             int *out_added) {
    *out_added = 0;
    mi_image im;
    if (mi_wrap(*pbuf, *psize, &im) != 0) {
        fprintf(stderr, "not a readable 64-bit Mach-O\n");
        return MR_REFUSED;
    }
    struct mach_header_64 *hdr = im.hdr;

    struct mv_scan scan = { UINT32_MAX, 0 };
    mi_each_lc(&im, mv_scan_lc, &scan);

    if (scan.has_version_min) {
        printf("LC_VERSION_MIN_MACOSX already present; nothing to do.\n");
        return 0;
    }

    uint32_t lc_end   = sizeof(*hdr) + hdr->sizeofcmds;
    uint32_t need_end = lc_end + (uint32_t)sizeof(struct version_min_command);
    /* Two ways "no room" is true that growing cannot cure, both refused
     * before anything is written: no section anywhere has a nonzero file
     * offset (scan.first_sect_off is still its UINT32_MAX sentinel, and a
     * write would go off whatever end the buffer has), or the command would
     * run past the buffer itself -- mi_wrap validates load commands, not
     * section file ranges, so first_sect_off is an untrusted value read
     * straight from the file. Fixed after a real heap overflow: a 104-byte
     * file (header + one LC_SEGMENT_64, nsects=0) hit exactly the first case
     * and wrote 16 bytes past a buffer whose allocation was exactly
     * file-sized; see tests/leaf-tool-crashes.sh. */
    if (scan.first_sect_off == UINT32_MAX || need_end > *psize) {
        fprintf(stderr, "no room for LC_VERSION_MIN_MACOSX\n");
        return MR_REFUSED;
    }
    /* The third way is the ordinary one -- the pad before the first section
     * is too small -- and that is mg_ensure_pad's to decide, grow or refuse,
     * the same as for every other load-command edit. */
    if (need_end > scan.first_sect_off) {
        if (mg_ensure_pad(pbuf, psize, need_end, allow_grow, "LC_VERSION_MIN_MACOSX") != 0) {
            fprintf(stderr, "no room for LC_VERSION_MIN_MACOSX\n");
            return MR_REFUSED;
        }
        hdr = (struct mach_header_64 *)*pbuf;   /* growth reallocated the buffer */
    }

    struct version_min_command *vm = (struct version_min_command *)(*pbuf + lc_end);
    memset(vm, 0, sizeof(*vm));
    vm->cmd = LC_VERSION_MIN_MACOSX;
    vm->cmdsize = sizeof(*vm);
    vm->version = (10 << 16) | (9 << 8);   /* 10.9.0 */
    vm->sdk     = (10 << 16) | (9 << 8);
    hdr->ncmds++;
    hdr->sizeofcmds += sizeof(*vm);
    *out_added = 1;
    return 0;
}
```

The "no room for LC_VERSION_MIN_MACOSX" line stays on every refusal: `tests/leaf-tool-crashes.sh:136` greps it.

- [ ] **Step 5: Change `mv_add_version_min` in `src/version_min.c`**

Change the signature to `int mv_add_version_min(const char *path, int allow_grow)`, and replace its tail — from `/* The edit itself, in memory; ...` through the end of the function — with:

```c
    /* The edit itself, in memory; what is left here is the file around it.
     * mi_release first: growing may reallocate the buffer, and the image
     * wrapper must not be left owning a pointer that realloc moved. */
    size_t fsize = im.size;
    uint8_t *buf = mi_release(&im);
    int added = 0;
    int rc = mv_add_version_min_image(&buf, &fsize, allow_grow, &added);
    if (rc != 0 || !added) {
        free(buf);
        close(fd);
        return rc;
    }

    /* A grown image is larger than the file it was read from; writing it
     * from offset 0 extends the file. The write is in place, through the
     * descriptor the race guard above checked -- a failed write can leave
     * the file partly written, exactly as before growth existed. */
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    lseek(fd, 0, SEEK_SET);
    if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("write"); free(buf); close(fd); return MR_FAIL; }
    close(fd);
    printf("Added LC_VERSION_MIN_MACOSX 10.9 (ncmds=%u, sizeofcmds=%u)\n",
           hdr->ncmds, hdr->sizeofcmds);
    free(buf);
    return 0;
}
```

Read the rest of the function first and keep every earlier line (open, fstat, mi_open, the race guard) unchanged.

- [ ] **Step 6: `cmd_minos` and `edit` call the new signatures**

In `cli/macho9.c`, `cmd_minos`: `return mv_add_version_min(path, 0);` (Task 4 replaces the 0).

In `src/edit.c`, the `MS_VERSION_MIN` arm becomes:

```c
    case MS_VERSION_MIN: {
        /* ms_parse accepts only 10.9, and 10.9 is the only floor this core
         * writes; the parser's value check is the one place that says so.
         * allow-grow reaches this statement: when the pad is short, growing
         * it is mg_ensure_pad's decision, the same as for dylib and rpath. */
        mi_image im;
        int added = 0;
        if (me_view(*pbuf, *psize, &im, path, log) != 0) return MR_REFUSED;
        int rc = mv_add_version_min_image(pbuf, psize, s->allow_grow, &added);
        /* Whether it appended a command or found one already there, as the
         * core reports it through `added`. The already-there case is on
         * stdout, where the core has always printed it; the append prints
         * nothing there, because its stdout line belongs to `macho9 minos`,
         * which edit does not call. */
        if (rc == 0 && verbose && added)
            me_say(log, "      appended LC_VERSION_MIN_MACOSX 10.9\n");
        return rc;
    }
```

`im` is used only for `me_view`'s validation; the core wraps `*pbuf` itself. If the compiler warns that `im` is unused after `me_view`, keep `me_view` and mark it `(void)im;` rather than dropping the check.

- [ ] **Step 7: Correct `src/edit.h`'s DIRECTIVES paragraph**

Replace lines 146-153 ("allow-grow covers only `dylib` and `rpath` statements ... so they never need it.") with:

```c
 * allow-grow covers the statements whose load commands can outgrow the
 * header pad: `dylib`, `rpath` and `version-min set`. For them, when the pad
 * is short, growing it is mg_ensure_pad's decision (src/grow.h) instead of a
 * refusal. It does not cover `fixups set classic`: growth refuses an image
 * that still has chained fixups, since chained pointers encode offsets from
 * the image base that growing moves -- and the conversion removes three
 * commands (up to 56 bytes) before adding its 48. So on a chained image
 * nothing can grow until `fixups set classic` has run: put it first.
 * Growth works only on a 64-bit PIE executable. `segment rename` and
 * `load-command delete` never add bytes to the load commands, so they never
 * need it.
```

- [ ] **Step 8: Run the tests and watch them pass**

```bash
cmake --build $B && sh tests/cli_test.sh $B 2>&1 | grep -c "^FAIL"; ctest --test-dir $B
```

Expected: 0 FAIL lines; ctest passes; no warnings.

- [ ] **Step 9: Prove the edit test can fail**

Temporarily change `s->allow_grow` to `0` in the MS_VERSION_MIN arm, rebuild, and confirm `edit: version-min set with allow-grow grows the header and succeeds` fails. Revert. Record the mutation and output in the task report.

- [ ] **Step 10: Commit**

```bash
sh tests/characterize.sh $B check
git add src/version_min.h src/version_min.c src/edit.c src/edit.h cli/macho9.c tests/cli_test.sh
git commit -m "feat: version-min set grows the header pad under allow-grow"
```

---

### Task 4: `macho9 minos --allow-grow`, and the docs say what is true

**Files:**
- Modify: `cli/macho9.c` — `cmd_minos`, its dispatch (`:1238-1241`), the file-header grammar (`:14`), usage (`:375`), `--capabilities` (`:334`)
- Modify: `README.md` — the `grow`/`allow-grow` passage (`:233-236`), the directive table (`:249-252`), and Limits (`:294-297`)
- Modify: `tests/cli_test.sh` — the `minos` and wrapper cases, and the capabilities check

**Interfaces:**
- Consumes: `mv_add_version_min(const char *path, int allow_grow)` from Task 3.
- Produces: `macho9 minos FILE 10.9 [--allow-grow]`; `verb minos versions=10.9 flags=allow-grow`.

- [ ] **Step 1: Write the failing tests**

After the Task 3 block in `tests/cli_test.sh` (it reuses `$T/vm_tight`):

```sh
# macho9 minos takes --allow-grow, after the version, as dylib/rpath take
# their flags; without it the verb refuses exactly as before.
cp "$T/vm_tight" "$T/vm_m"
rc=0
"$MACHO9" minos "$T/vm_m" 10.9 >/dev/null 2>"$T/vm_m_no.err" || rc=$?
[ "$rc" -eq 1 ] && ok "minos: without --allow-grow a short pad is refused (1)" \
    || bad "minos --allow-grow" "without the flag: expected 1, got $rc: $(cat "$T/vm_m_no.err")"
grep -q "allow-grow" "$T/vm_m_no.err" \
    && ok "minos: ... and the refusal names allow-grow" \
    || bad "minos --allow-grow" "no allow-grow remedy in: $(cat "$T/vm_m_no.err")"
rc=0
"$MACHO9" minos "$T/vm_m" 10.9 --allow-grow >/dev/null 2>"$T/vm_m_yes.err" || rc=$?
[ "$rc" -eq 0 ] && ok "minos: --allow-grow grows the header and adds the command" \
    || bad "minos --allow-grow" "with the flag: expected 0, got $rc: $(cat "$T/vm_m_yes.err")"
"$MACHO9" info "$T/vm_m" | grep -q "LC_VERSION_MIN_MACOSX" \
    && ok "minos: --allow-grow: LC_VERSION_MIN_MACOSX is present" \
    || bad "minos --allow-grow" "no LC_VERSION_MIN_MACOSX after the grow"
"$MACHO9" verify "$T/vm_m" >/dev/null 2>"$T/vm_m_verify.err" \
    && ok "minos: --allow-grow: the grown file passes macho9 verify" \
    || bad "minos --allow-grow" "verify refused: $(cat "$T/vm_m_verify.err")"
"$MACHO9" minos "$T/vm_m" 10.9 --bogus >/dev/null 2>&1 \
    && bad "minos" "an unknown flag was accepted" \
    || ok "minos: an unknown flag is a usage error"

# The historical add_version_min never grew, so its wrapper still refuses.
cp "$T/vm_tight" "$T/vm_w"
rc=0
"$BIN/add_version_min" "$T/vm_w" >/dev/null 2>"$T/vm_w.err" || rc=$?
[ "$rc" -ne 0 ] && ok "add_version_min: still refuses a short pad (never grows)" \
    || bad "add_version_min" "the wrapper grew or succeeded: $(cat "$T/vm_w.err")"

echo "$caps" | grep -q "^verb minos versions=10.9 flags=allow-grow$" \
    && ok "capabilities: minos advertises allow-grow" \
    || bad "capabilities minos" "expected 'verb minos versions=10.9 flags=allow-grow': $(echo "$caps" | grep '^verb minos')"
```

`$caps` is the `--capabilities` output captured near the top of `cli_test.sh`; confirm the variable name there before relying on it.

- [ ] **Step 2: Run and watch them fail**

```bash
sh tests/cli_test.sh $B 2>&1 | grep -E "minos|add_version_min|capabilities minos"
```

Expected: `--allow-grow grows the header` FAILs (the verb's `argc != 4` rejects the flag) and the capabilities line FAILs.

- [ ] **Step 3: Add the flag**

`cmd_minos` takes the flag:

```c
static int cmd_minos(const char *path, const char *version, int allow_grow) {
    if (strcmp(version, "10.9") != 0) {
        fprintf(stderr, "macho9 minos: only 10.9 is supported by this build (got '%s')\n", version);
        return EX_REFUSED;
    }
    return mv_add_version_min(path, allow_grow);
}
```

The dispatch accepts exactly `minos FILE 10.9` or `minos FILE 10.9 --allow-grow`:

```c
    if (strcmp(verb, "minos") == 0) {
        int allow_grow = (argc == 5 && strcmp(argv[4], "--allow-grow") == 0);
        if (argc != 4 && !allow_grow) {
            fprintf(stderr, "usage: %s minos FILE 10.9 [--allow-grow]\n", argv[0]);
            return EX_FAIL;
        }
        return cmd_minos(argv[2], argv[3], allow_grow);
    }
```

Update the file-header grammar line (`:14`) and the usage text (`:375`) to `macho9 minos FILE 10.9 [--allow-grow]`, and the capabilities line (`:334`) to:

```c
    printf("verb minos versions=10.9 flags=allow-grow\n");
```

`compat/add_version_min.sh` is not touched: it runs `macho9 minos FILE 10.9` and never passes the flag.

- [ ] **Step 4: Correct the README**

1. The `grow`/`allow-grow` sentence (`:233-236`): replace "`allow-grow`, below, is the directive that lets a `dylib`/`rpath` statement grow the pad on its own as a side effect" with "`allow-grow`, below, is the directive that lets a `dylib`, `rpath` or `version-min set` statement grow the pad on its own as a side effect".
2. The `allow-grow` directive row (`:249-252`): append, keeping the table's layout, "Covers dylib, rpath and version-min set; not fixups set classic -- nothing grows while the image still has chained fixups, so put fixups set classic first."
3. Limits (`:294-297`), replace the `allow-grow` bullet with:

```markdown
- **`allow-grow` reaches `dylib`, `rpath` and `version-min set`** — the
  statements whose load commands can outgrow the header pad — and only on a
  64-bit PIE executable. It does not reach `fixups set classic`: growth
  refuses an image that still has chained fixups, and the conversion frees
  more room than it uses. So on a chained image nothing can grow until
  `fixups set classic` has run; put it first. `segment rename` and
  `load-command delete` never need it, since neither adds bytes to the load
  commands.
```

4. Wherever the README documents `macho9 minos`, add `[--allow-grow]`.

- [ ] **Step 5: Run the tests and watch them pass**

```bash
cmake --build $B && sh tests/cli_test.sh $B 2>&1 | tail -1 && ctest --test-dir $B
```

Expected: `cli_test: 0 failure(s)`; ctest passes.

- [ ] **Step 6: Prove the minos test can fail**

Temporarily make `cmd_minos` call `mv_add_version_min(path, 0)`, rebuild, confirm `minos: --allow-grow grows the header and adds the command` fails. Revert. Record it.

- [ ] **Step 7: Commit**

```bash
sh tests/characterize.sh $B check
git add cli/macho9.c README.md tests/cli_test.sh
git commit -m "feat: macho9 minos --allow-grow"
```

---

## Self-review

**Spec coverage.** "One place decides whether there is room" → Tasks 1 (the function) and 2 (mr_process_thin's call). "The messages say the true remedy" → Task 1 Steps 5-6 (no-room, chained, dylib/bundle, non-PIE, plus the classify suffix that named `change_dylib`'s flags, which the spec's "name the real remedy" covers). "version-min grows" → Task 3. "Front-ends" → Task 3 (`edit`), Task 4 (`minos`, capabilities, wrapper unchanged). "Documentation" → Task 3 Step 7 (`edit.h`), Task 4 Step 4 (README, usage). "Testing" → Task 1 (unit, including the chained case in `grow_test.c` because cli_test's chained fixture is `MH_DYLIB`), Task 2 (old-vs-new), Tasks 3-4 (tight fixture, `edit`, `minos`, wrapper). The spec's three mutations → Task 1 Step 8 (removing the chained-fixups case; `mg_ensure_pad` ignoring `allow_grow`) and Task 3 Step 9 (dropping `allow_grow` from `edit`'s version-min arm). Task 4 Step 6 adds a fourth, for the `minos` flag. "For item 5" → a note, no task.

**Placeholders.** None; every code step carries its code.

**Type consistency.** `mg_ensure_pad(uint8_t **, size_t *, uint32_t, int, const char *)` is spelled the same in Tasks 1, 2 and 3. `mv_add_version_min_image(uint8_t **, size_t *, int, int *)` and `mv_add_version_min(const char *, int)` match between Task 3's header, definitions, and Task 4's caller.
