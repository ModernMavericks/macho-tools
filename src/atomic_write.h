/* atomic_write.h -- replace a file's content without ever leaving it
 * half-written.
 *
 * Extracted from change_dylib.c, where this logic first shipped, so that
 * `macho9 grow` could share it instead of carrying its own copy. Before this,
 * `macho9 grow` wrote its result via ftruncate()+write() directly into the
 * open file -- a write failing partway (disk full, killed mid-write) left
 * the file truncated with only part of the new content in it, exactly the
 * failure mode change_dylib's write_atomic() was written to rule out. The
 * two tools do the same thing (replace a Mach-O file's bytes on disk after
 * successfully rewriting it in memory) and had no reason to do it two
 * different ways, let alone one safer than the other.
 *
 * THAT WHOLE QUESTION IS GOING AWAY: a verb that writes an OUT of its own
 * replaces nothing, so wa_write_new (below) is what every converted verb --
 * `macho9 grow` included, now -- calls instead. src/edit.c is the last caller
 * wa_write_atomic has left.
 */

#ifndef MACHO9_ATOMIC_WRITE_H
#define MACHO9_ATOMIC_WRITE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* Write `size` bytes of `buf` as the new content of the file `path` refers
 * to. Two strategies, chosen by link count:
 *
 * ORDINARY CASE (the common one: a single hard link, `path` possibly a
 * symlink to it): atomic mkstemp()+rename(). `path` is realpath()'d FIRST so
 * the rename lands on the real target, never on `path` itself -- replacing a
 * symlink via rename would turn it into a plain file and leave the real
 * target (and everything else that follows the same symlink) unpatched.
 * macOS framework dylibs are exactly this shape (Foo.framework/Foo ->
 * Versions/A/Foo). The temp file is created in the resolved target's
 * directory, so the rename stays on one filesystem and is therefore atomic,
 * and every xattr on the original (quarantine, etc.) is copied onto it
 * before the rename. Either the OLD content (and its xattrs) is still there
 * afterward or the NEW content (and copied xattrs) is, in full -- never a
 * half-written or truncated file. `mode` sets the new file's permissions
 * (best-effort, via fchmod).
 *
 * HARD-LINK CASE (st_nlink > 1): rename() would give the resolved path a
 * FRESH inode, leaving every other name for that inode -- the sibling hard
 * links -- pointing at the old, unpatched content. There is no atomic way to
 * update every name for an inode at once, so this falls back to writing
 * through the existing inode (ftruncate+write), which gives up the
 * atomicity the ordinary case has: a write failing partway leaves the file
 * truncated. This is strictly better than what it replaces (which always
 * took this path), never worse.
 *
 * Returns 0 on success, non-zero (with a message on stderr) on failure. On
 * failure the ordinary case leaves `path` untouched (the temp file is
 * unlinked); the hard-link case's failure mode is whatever partial write it
 * managed, per the paragraph above.
 */
int wa_write_atomic(const char *path, mode_t mode, const uint8_t *buf, size_t size);

/* wa_write_new's two failure codes. */
#define WA_IS_INPUT 1   /* `out` is `in`; nothing written */
#define WA_FAILED   2   /* an I/O or allocation failure; the reason is on stderr */

/* 1 if `out` names the same file as `in` -- the same path once both are
 * resolved, or, when `out` exists, the same device and inode, which catches a
 * symlink to `in` and a hard link to it -- else 0. For a caller that wants to
 * refuse before doing any work; wa_write_new checks again at the write. */
int wa_is_input(const char *in, const char *out);

/* Write `size` bytes of `buf` as the file `out`, never touching `in`.
 * Returns WA_IS_INPUT, writing nothing, when wa_is_input(in, out). Otherwise
 * writes a temp file in `out`'s resolved directory -- a symlink at `out` is
 * followed to its target, not replaced -- gives it `in`'s mode, `in`'s owner
 * (best-effort: needs privilege) and every extended attribute `in` carries,
 * fsyncs it, and renames it onto `out`. So `out` is either what it was or the
 * whole new content, never a partial file. Returns 0, WA_IS_INPUT, or
 * WA_FAILED with the reason on stderr; on any non-zero return `out` is as it
 * was and no temp file remains. */
int wa_write_new(const char *in, const char *out, const uint8_t *buf, size_t size);

#endif /* MACHO9_ATOMIC_WRITE_H */
