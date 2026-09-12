#ifndef MACHOTOOL_REWRITE_H
#define MACHOTOOL_REWRITE_H
/*
 * mr_ -- rewriting a Mach-O's dylib load commands and LC_RPATHs.
 *
 * This is change_dylib's whole operation set, lifted out of that tool's
 * main() so it is a library function rather than a program. cli/macho9.c's
 * `dylib`/`rpath`/`lc`/`segment` verbs are its front-end
 * (-replace/-delete/-append/-insert/-reexport, plus a segment rename shared
 * with src/segname.h), through mr_apply_file; src/edit.c's edit scripts are
 * the other, through mr_apply_image, the same rewrite applied to an image
 * already in memory. The OLD grammar -- change_dylib's
 * -change/-delete/-reexport/-add/-insert/-strip-lc and the -*-rpath twins --
 * reaches exactly this code through compat/change_dylib.sh, the /bin/sh
 * wrapper that replaced compat/change_dylib.c, and compat/translate.sh, which
 * maps one grammar onto the other. macho9 used to fork and exec change_dylib
 * to get this work done; that made `change_dylib` a runtime dependency of
 * `macho9`, which is a cycle once change_dylib becomes a wrapper around
 * macho9. Sharing the code instead of the binary broke it, and is what made
 * the wrapper possible.
 *
 * The parsing stays in each front-end -- the two grammars are genuinely
 * different, and neither is this module's business. What crosses the boundary
 * is an mr_ops: the operation SET, already parsed, pointing at caller-owned
 * arrays.
 *
 * Every diagnostic these functions print is part of the contract, not an
 * implementation detail: both front-ends' output has to stay what
 * change_dylib's has always been (tests/change_dylib_test.sh and
 * tests/characterize.sh both pin it), so messages live down here, once,
 * rather than being re-emitted by each caller.
 *
 * LC_RPATH carries no library ordinal (mo_is_ordinal_lc, src/ordinals.h,
 * names the four dylib commands that do and LC_RPATH is not among them), so
 * unlike a dylib insert an rpath insert shifts nothing and needs no
 * renumbering at all -- it is purely a question of where in the table the new
 * command is emitted.
 *
 * LIBRARY ORDINALS. In a two-level-namespace image every undefined symbol
 * records which dylib it comes from, as a 1-based index into the dylib load
 * commands in load order. The index lives in two places: the nlist n_desc of
 * each undefined symbol, and the SET_DYLIB_ORDINAL opcodes of the LC_DYLD_INFO
 * bind/weak/lazy streams. Appending is safe because it only hands out new
 * indices, but INSERTING or DELETING shifts every later one.
 *
 * Leaving them stale does not produce a subtle bug so much as an unloadable
 * binary: the highest ordinal usually belongs to libSystem (dyld_stub_binder),
 * so after a deletion dyld rejects the image with "library ordinal (N) too big".
 * Where the shifted index does stay in range it is worse, because it silently
 * names a different library. Either way the rewrite has to renumber, so
 * inserts and deletes do, and a delete refuses outright if any symbol still
 * binds to the dylib being removed.
 */
#include <stdint.h>
#include <stddef.h>

#include "ordinals.h"

/* One dylib-path (or rpath) operation.
 *
 * new_path == NULL  -- delete the command naming old_path
 * new_path == ""    -- leave the path alone (used with reexport, which only
 *                      changes the command's KIND)
 * otherwise         -- rewrite the path to new_path, growing the command if
 *                      the longer string needs it
 *
 * `reexport` promotes LC_LOAD_DYLIB to LC_REEXPORT_DYLIB and is meaningless
 * for an rpath operation (LC_RPATH has only one kind), so mr_ops' rpath
 * arrays always leave it 0. */
typedef struct {
    const char *old_path;
    const char *new_path;
    int reexport;
} mr_change;

/* What a rewrite's ordinal renumbering did: the follow-up work an insert or
 * a delete of a dylib does unasked (LIBRARY ORDINALS, above), handed back so
 * a front-end can report it. Every field is copied from the one map and the
 * one renumbering the rewrite already made -- mo_map_build's map, with the
 * load command kind it saw at each old ordinal, and mo_map_apply's
 * mo_counts -- and nothing here is recomputed. */
typedef struct {
    int       done;                          /* 1 once the rest is filled in */
    int       n;                             /* ordinals the image had: 1..n */
    int       inserted;                      /* the new LC_LOAD_DYLIBs took
                                              * ordinals 1..inserted */
    int       old_to_new[MO_MAX_DYLIBS + 1]; /* mo_map's: 0 = deleted */
    uint32_t  old_cmd[MO_MAX_DYLIBS + 1];    /* the kind at each old ordinal */
    mo_counts counts;                        /* what mo_map_apply changed */
} mr_renumbering;

/* Everything one run of the rewriter is being asked to do. Each array is
 * caller-owned and read-only for the duration of the call; a count of 0 means
 * that operation was not requested and its pointer is never dereferenced.
 *
 * Order within an array is the order the operations were given on the command
 * line, and it is observable: inserted dylibs become ordinals 1..n in the
 * order they appear here, and appended ones land after every existing
 * dependency in the order they appear here. The same holds for LC_RPATHs,
 * where load order is not an ordinal but a SEARCH ORDER -- dyld takes the
 * first rpath that resolves -- so rpath_inserts land ahead of every LC_RPATH
 * the image already had and rpath_appends land behind them. */
typedef struct {
    const mr_change *dylib_changes;    /* rewrite/delete/reexport a dependency */
    int              n_dylib_changes;
    const char *const *dylib_appends;  /* brand-new LC_LOAD_DYLIB, placed last */
    int              n_dylib_appends;
    const char *const *dylib_inserts;  /* brand-new LC_LOAD_DYLIB, placed first */
    int              n_dylib_inserts;
    const uint32_t  *strip_cmds;       /* whole load commands to drop, by LC_* */
    int              n_strip_cmds;
    const mr_change *rpath_changes;    /* rewrite/delete an LC_RPATH */
    int              n_rpath_changes;
    const char *const *rpath_appends;  /* brand-new LC_RPATH, searched LAST */
    int              n_rpath_appends;
    const char *const *rpath_inserts;  /* brand-new LC_RPATH, searched FIRST */
    int              n_rpath_inserts;
    /* Rename every LC_SEGMENT_64 named segment_rename_old -- and the copy of
     * the segment name each of its sections carries -- to segment_rename_new.
     * Both NULL means no rename was requested; the pair is scalar rather than
     * an array because the only grammar that spells it (macho9 segment FILE
     * OLD NEW) takes exactly one pair. The rename itself is mseg_rename_lc
     * (src/segname.h), shared with the rename_segment grammar. */
    const char      *segment_rename_old;
    const char      *segment_rename_new;
    /* OUT, one of the two fields here that are not instructions
     * (renumbering, next, is the other): if non-NULL, the rewriter ADDS to it the number of LC_SEGMENT_64s it actually
     * renamed -- summed over every slice of a fat container, and left alone
     * entirely when the rewrite is refused, since a refused rewrite renamed
     * nothing on disk.
     *
     * It exists because "how many matched" is not derivable from outside.
     * mseg_rename_lc matches with strncmp over the 16-byte segname field
     * (src/segname.h), and a segname is neither NUL-terminated nor free of
     * whitespace, so no front-end can recover the count by reading a printed
     * name back: an OLD longer than 16 bytes whose first 16 match, or a
     * segname containing a space, both defeat it. The old `rename_segment`
     * grammar needs the count for its one output line AND for its exit 2 when
     * nothing matched, so the count has to come from the code that did the
     * matching. cli/macho9.c's `segment` verb reports it. */
    int             *segment_renamed;
    /* OUT, filled on the same terms as segment_renamed: only past every
     * gate, never by a refused rewrite. If non-NULL and the rewrite renumbered library
     * ordinals -- it inserted or deleted a dylib -- *renumbering is
     * OVERWRITTEN with what that renumbering did (see mr_renumbering) and
     * its `done` set to 1. Otherwise it is left alone, so a caller zeroes it
     * first and reads `done`. Overwritten rather than added to, because a map
     * does not sum: for a fat container each slice that renumbers replaces
     * the previous slice's. src/edit.c, the only caller that sets it, edits
     * thin images only. Being an OUT, it is not consulted by
     * mr_is_rename_only, any more than segment_renamed is. */
    mr_renumbering  *renumbering;
    /* If non-zero, mr_apply_file refuses (returns MR_REFUSED, below) when
     * mr_report_unmatched finds that any dylib_changes/rpath_changes/
     * strip_cmds entry matched nothing -- the same report that otherwise just
     * goes to stderr, promoted from an FYI to a refusal, the way `ld` and
     * `gas`'s own --fatal-warnings promote a warning to an error. NOTHING IS
     * WRITTEN when it fires: mr_apply_file decides this verdict before its
     * wa_write_new, so a refused run leaves `out` exactly as it was -- absent,
     * if it was absent -- and `path`, which it never writes at all, likewise.
     * That used to hold only by accident: the write came last, and an all-miss
     * run had `modified == 0` and so wrote nothing.
     *
     * WHAT IT DOES NOT CATCH, and why the line is drawn there: this asks
     * "did anything in the image MATCH this operation", never "did this
     * operation act". `dylib f -replace X A -replace X B` matches X twice,
     * so neither entry is unmatched and --fatal-warnings is silent, even
     * though only the first -replace can act -- the second is shadowed. The
     * predicate is deliberately the matched-not-acted one: the counting site
     * (src/rewrite.c, mr_build_lcs_lc's "No break" comment) explains that a
     * -delete and a -change may legitimately name the same old_path, and
     * that a counting rule of "only the operation that ACTED" reports the
     * -delete of `-replace X N -delete X` as a false miss. Loosening this is
     * how that false miss comes back, so a shadowed operation stays silent.
     *
     * cli/macho9.c's `dylib`, `rpath` and `lc` verbs set this from
     * --fatal-warnings, and src/edit.c sets it on every statement it lowers
     * to an mr_ops when the edit script says `fatal-warnings`; `segment` and
     * `retag-swift` don't take a list of operations that could miss, so they
     * have nothing to parse a --fatal-warnings flag into. src/edit.c applies
     * one statement at a time to an image it writes only at the end, so a
     * refusal there discards the whole run -- the same "nothing written"
     * answer mr_apply_file now gives. Declared
     * before allow_grow, not after, so allow_grow stays the LAST field --
     * see the layout tripwire next to mr_is_rename_only in rewrite.c, which
     * checks the last field's offset precisely so that inserting a new
     * field ahead of it keeps tripping the check on the next such edit
     * too. */
    int              fatal_unmatched;
    int              allow_grow;       /* may enlarge the header pad (mg_grow_header) */
} mr_ops;

/* How many times one operation may repeat in a single run. THREE call sites
 * accumulate into fixed-size C arrays sized from these two macros; a fourth
 * enforces the identical numeric cap from its own separately-declared shell
 * constant, since it cannot include this header. All four refuse (or, for
 * mr_apply_file's own arrays, must never be handed more than) the same count
 * -- `change_dylib -delete ... x33`, `fix_macho -change ... x33` and
 * `macho9 dylib -delete ... x33` all agree about being too many -- each in
 * its own wording, since none of the grammars spell the operations the same
 * way:
 *
 *   cli/macho9.c's own dylib/rpath parser checks the count inline and prints
 *     "macho9 <verb>: too many <flag> operations (max N)", naming ITS OWN
 *     flag spelling (`-append`, not change_dylib's `-add`) -- see the
 *     comment at that call site for why the wording is deliberately not
 *     shared with the other two. This is the ONLY call site that actually
 *     constructs an mr_ops and passes it to mr_apply_file below -- see that
 *     function's own comment for why that makes it load-bearing, not just
 *     one front-end among several.
 *   compat/fix_macho.c's FM_ROOM macro used to be here too, reusing this
 *     MR_MAX_OPS rather than spelling out a second 32 and printing "too many
 *     <flag> (max N)" in fix_macho's own words. That file is GONE: fix_macho
 *     is a /bin/sh wrapper, its argv is accumulated in shell, and its two
 *     caps moved into compat/translate.sh's mt_room alongside change_dylib's
 *     (with a literal 16 of their own for -rename_seg, which no shared header
 *     has an opinion about). Recorded here because the numeric agreement was
 *     the reason this list mentioned that file at all.
 *   compat/translate.sh's mt_room -- CD_ROOM revived again, since
 *     change_dylib.c is gone -- accumulates the OLD grammar's argv into a
 *     shell variable rather than a C array, capped by its own literal
 *     MT_MAX_OPS=32 (not derived from MR_MAX_OPS: a /bin/sh script cannot
 *     include this header), and refuses at the identical count, in
 *     change_dylib's own historical words ("too many <flag> (max N)"),
 *     before ever emitting a `macho9` command line.
 *   mr_apply_file (src/rewrite.c) declares its own per-operation hit-count
 *     arrays -- an mr_hits (below): int[MR_MAX_OPS] for dylib/rpath,
 *     int[MR_MAX_STRIP] for strip -- sized from these same two macros, but
 *     does NOT itself check `ops->n_dylib_changes`/`n_rpath_changes`/
 *     `n_strip_cmds` against them. See mr_apply_file's own comment for the
 *     precondition this leaves on its caller.
 *
 * NOT a cap on an edit script. src/edit.c lowers each statement to an mr_ops
 * holding exactly one operation and hands it to mr_apply_image, so its
 * mr_hits never counts past index 0, however many statements the script has
 * -- the statement array itself is sized from the parsed script
 * (src/script.h). */
#define MR_MAX_OPS   32
#define MR_MAX_STRIP 16

/* Returned by mr_apply_file in place of 0 when ops->fatal_unmatched turned
 * "an operation matched nothing" into a refusal (see that field's own
 * comment above) -- one of several considered refusals this function can
 * return; see its own comment below for the rest. Deliberately equal to
 * cli/macho9.c's own EX_REFUSED: that is the ONLY caller today, `dylib`/
 * `rpath`/`lc` all forward mr_apply_file's return value verbatim (`return
 * mr_apply_file(path, out, &ops);`), and this way that forwarding keeps meaning
 * what --capabilities documents without the caller having to translate a
 * rewrite-library code into its own exit-code vocabulary. cli/macho9.c
 * enforces this equality as a build failure, not just this comment -- see
 * the typedef next to EX_REFUSED's definition.
 *
 * Deliberately 1, not 2: `diff`/`grep`/`cmp` all reserve their HIGHEST code
 * for "the tool could not do its job" and use a lower one for "a normal,
 * expected, non-success answer" -- the opposite of what this codebase shipped
 * first. binutils has no equivalent at all (it returns a flat 0 or 1 and
 * never distinguishes a considered refusal from a genuine failure), so this
 * is not matching an existing convention so much as choosing the one that
 * generalizes past this repo's own history. Nothing outside this repo has
 * ever run the compat wrappers this couples to, and `edit` (a later feature)
 * is what starts to make that numbering a real, depended-upon contract --
 * so this is the last point at which it can change for free. */
#define MR_REFUSED 1

/* Returned by mr_apply_file (and by mv_add_version_min, src/version_min.c,
 * the same arrangement one level down) for a genuine operational failure:
 * open, fstat, read or write itself failing, or a checked allocation that
 * src/rewrite.c's own drivers make (mr_apply_file's fat-path read buffer) or
 * that mi_open, mfat_parse or mfat_rewrite make one level down -- for the
 * file itself, or, in mfat_rewrite's case, for the tracking arrays, slice
 * copies and reassembly buffer that splitting a fat file needs first
 * (mv_add_version_min also returns it when wa_write_new cannot produce its
 * output -- see that function's own comment). NEVER for a considered
 * refusal -- a site that
 * examined the bytes and declined, however it phrases that on stderr, is
 * MR_REFUSED, not this. And an allocation failure INSIDE mg_grow_header or
 * mg_plausible deliberately does not come here either: it is folded into
 * MR_ERROR same as every other reason either one refuses, and so surfaces
 * as MR_REFUSED. See mr_apply_file's own comment below for the dividing
 * line, that exception, and examples of each. Named the same way as
 * MR_REFUSED, and cli/macho9.c's EX_FAIL is required to equal it for the
 * same reason EX_REFUSED is required to equal MR_REFUSED -- see the typedef
 * next to EX_FAIL's own definition. */
#define MR_FAIL 2

/*
 * Apply `ops` to the Mach-O at `path` and write the result as the NEW file
 * `out`. `path` is only ever read -- it is opened O_RDONLY and never written,
 * whatever happens -- so the "in place" this function used to do is now the
 * caller's business (compat/macho9-compat.sh's install path does it with a
 * temp and an mv). Handles both a thin 64-bit Mach-O and a classic
 * (32-bit-offset fat_arch) fat container, whose slices are each rewritten and
 * then reassembled; a 64-bit fat container (fat_arch_64) is refused
 * explicitly, and a fat slice this rewriter does not understand is passed
 * through byte-for-byte.
 *
 * Returns 0 on success -- including the "nothing matched" case, where `out` is
 * written anyway, as a copy of `path`: a 0 exit means `out` IS the answer, so
 * it has to exist either way. On any nonzero return `out` is as it was (or
 * still absent) and nothing was written: every refusal, the unmatched verdict
 * included, happens before the single wa_write_new at the end. Failure is one
 * of two codes, matching cli/macho9.c's own EX_REFUSED/EX_FAIL split
 * (this function's caller forwards whichever one it gets verbatim, so the
 * split has to be made correctly here, not patched up one level out):
 *
 *   MR_REFUSED (1) -- a CONSIDERED refusal: this function (or a primitive it
 *     calls -- mi_open, mfat_parse, mg_first_sect_off, mo_map_build,
 *     mr_build_lcs, mg_grow_header, mo_map_validate, mo_map_apply,
 *     mg_plausible) examined the bytes and declined on purpose. "Examined"
 *     covers more than "read the input Mach-O": a result that fails
 *     validation, new load commands that don't fit and can't be grown, a
 *     rewrite whose own cross-check disagrees with what it just built,
 *     reassembled fat slices that would overlap, an unsupported 64-bit fat
 *     container, "not a 64-bit Mach-O" in any of its forms, and a final
 *     mg_plausible verify that fails are all considered refusals, not
 *     operational failures -- even though several of these are reached
 *     through a helper's own nonzero return rather than a check written out
 *     here. ONE EXCEPTION: mg_grow_header and mg_plausible each fold an
 *     allocation failure of their own into the same signal they use for
 *     every other refusal (grow.c), and this function cannot tell that case
 *     apart from the rest -- see src/rewrite.c, the comment in mr_apply_image
 *     (below) where mr_process_thin's MR_ERROR becomes MR_REFUSED, for why
 *     that stays folded in rather than being split out to MR_FAIL,
 *     and for why it is not confined to --allow-grow runs.
 *   MR_FAIL (2) -- a genuine operational failure: open, fstat, read or write
 *     failing (this function's own, or mi_open's/mfat_parse's), wa_write_new
 *     failing to produce `out`, or a
 *     checked allocation src/rewrite.c's own drivers make (see MR_FAIL's
 *     definition above) or mi_open/mfat_parse make for the file/table.
 *     Nothing about the INPUT was in question; the environment (a
 *     permission, a full disk, an exhausted heap) was.
 *
 * THERE IS NO EXCEPTION to "every refusal happens before the write" any more,
 * and that is deliberate: ops->fatal_unmatched's refusal used to arrive after
 * the write, because the write was the last thing this function did and an
 * all-miss run wrote nothing anyway (`modified` was 0). Now that `out` is
 * written even when nothing changed, that ordering would create `out` and THEN
 * return MR_REFUSED -- so the verdict is decided first and the write happens
 * only when the run will return 0. The report itself is on stderr
 * (mr_report_unmatched), so a successful run's stdout is unaffected by the
 * move.
 *
 * PRECONDITION, unenforced here: `out` must not name `path`. cli/macho9.c
 * refuses that up front, in each verb's own words, before any file is read
 * (see mt_bad_out there); this function does not check again, because
 * wa_write_new does -- so an unchecked caller gets MR_FAIL and an unwritten
 * input rather than a silently rewritten one, just later and in
 * atomic_write.c's wording.
 *
 * PRECONDITION, unenforced here: `ops->n_dylib_changes` and
 * `ops->n_rpath_changes` must each be <= MR_MAX_OPS, and
 * `ops->n_strip_cmds` must be <= MR_MAX_STRIP. This function keeps its own
 * per-operation hit-count arrays on the stack, sized exactly from those two
 * macros, to report (on stderr) which operations matched nothing; it trusts
 * the caller for the bound the same way the rest of this module already
 * trusts mr_ops's arrays to be caller-owned and caller-sized. cli/macho9.c
 * is the only caller today, and enforces the identical cap itself before
 * ever building an mr_ops (see MR_MAX_OPS's own comment) -- but that
 * enforcement lives in the caller, not in this library, so a future or
 * different caller that skips it turns an over-long array into a stack
 * overflow here, not a diagnostic. */
int mr_apply_file(const char *path, const char *out, const mr_ops *ops);

/* Per-operation hit counts: how many load commands each entry of an mr_ops'
 * dylib_changes, rpath_changes and strip_cmds matched, index for index. They
 * are ADDED to, never assigned, because a fat file's slices each add their
 * own matches to one total -- an operation that matched in one slice and not
 * another has matched. So the caller zeroes an mr_hits once per image (or
 * per fat file) and not between slices. Sized from the same two macros that
 * bound mr_ops' arrays (see mr_apply_file's PRECONDITION above). */
typedef struct {
    int dylib[MR_MAX_OPS];
    int rpath[MR_MAX_OPS];
    int strip[MR_MAX_STRIP];
} mr_hits;

/*
 * mr_apply_file's thin-image step, without the file: apply `ops` to the thin
 * 64-bit Mach-O already in memory at *pbuf (*pfsize bytes), and write
 * nothing. mr_apply_file reads a thin file and hands its buffer here;
 * src/edit.c calls this once per edit-script statement, against the one
 * image it verifies and writes itself after the last statement. There is one
 * rewrite either way -- this is that rewrite, not a copy of it.
 *
 * *pbuf may be realloc'd (allow_grow reaches mg_grow_header). On return,
 * success or not, *pbuf and *pfsize name the buffer the caller owns and must
 * free(). After a failure its contents are unspecified -- the new load
 * commands may already have been committed when a later check refused -- so
 * a caller that sees a refusal must discard the buffer, never write it.
 *
 * Prints what mr_apply_file's own thin path prints apart from its closing
 * "Wrote OUT" line (there is no file here to have written), with `label` in
 * place of the path: the header-pad and "updated"/"nothing to change"
 * progress lines on stdout, and each refusal's reason on stderr. It does NOT
 * report which operations matched nothing, and does not act on
 * ops->fatal_unmatched: the hit counts are ADDED to `hits` (see mr_hits), and
 * mr_unmatched_verdict, below, does both once the caller has finished with
 * the image.
 *
 * Returns 0, with *out_modified saying whether anything changed, or
 * MR_REFUSED. Never MR_FAIL: there is no I/O here, and every allocation
 * failure it can report at all is the one mr_apply_file's own comment
 * describes as folded into MR_REFUSED (inside mg_grow_header or
 * mg_plausible). Its two new_lcs callocs are not checked at all, the same as
 * when mr_apply_file reaches them.
 *
 * PRECONDITION: the same array bound as mr_apply_file's, for the same
 * reason -- `hits` holds MR_MAX_OPS/MR_MAX_STRIP counters. src/edit.c meets
 * it by construction: each statement lowers to an mr_ops holding exactly one
 * operation. */
int mr_apply_image(uint8_t **pbuf, size_t *pfsize, const char *label,
                   const mr_ops *ops, int *out_modified, mr_hits *hits);

/* After a successful rewrite, report on stderr every dylib_changes/
 * rpath_changes/strip_cmds entry that matched nothing according to `hits`
 * (the "macho9: ... matched nothing" lines), and decide what that means:
 * MR_REFUSED if at least one matched nothing and ops->fatal_unmatched is set,
 * otherwise 0. Only after a SUCCESSFUL rewrite: a refused one may have
 * stopped before a single comparison ran, and its hit counts mean nothing.
 * Both callers ask BEFORE writing anything: mr_apply_file just before its
 * wa_write_new, so a refusing verdict leaves `out` unwritten; src/edit.c after
 * each statement, against an image it writes only at the end. */
int mr_unmatched_verdict(const mr_ops *ops, const mr_hits *hits);

#endif /* MACHOTOOL_REWRITE_H */
