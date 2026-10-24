# Files, saves, crash handling, installer: comparison for Phantom Dust recomp

Ours = C:\recomp\phantom-dust-re. T = takedawn-xboxrecomp (Burnout 3 fork), U = upstream-xboxrecomp,
TD = takedawn game layer, DJ = defjam, MC = mcdub. Paths relative to R. Read-only analysis; nothing built or run.
Line numbers are from the files as read.
Licence: xboxrecomp and its forks are MIT (xbox_crash.c cites BLiNX2 MIT and hl2 main.c). Game-layer repos (TD/DJ/MC): licence not verified per file; check LICENSE before copying.

Fork-state finding: MC and DJ kernel_file.c are OLDER than ours/upstream (no dir-race fix, no FATX overwrite). Their value is in the installer layers.
The NtQueryDirectoryFile slot race fix (in_use claimed under lock, ours kernel_file.c:640-720) is NOT present in U, T, MC or DJ. Ours is ahead there.

---

## A. SAVE INTEGRITY

### A1 (P1) Save guard: write to `<name>.~new`, flush, atomic rename on last close
- Source: T src/kernel/kernel_saveguard.c (1206 lines): commit() 549-616, xbox_saveguard_open 684-750, h_replace 277-316 (MoveFileEx REPLACE_EXISTING|WRITE_THROUGH, 25 retries x 20 ms), sweep of orphans 1064-1120, xbox_SaveGuardEnable 1162-1206, exit wait 1122-1160. Header T src/kernel/kernel_saveguard.h. Hooks in T kernel_file.c: 119 (guard_mode), 373-374, 418, 444, 529, 551, 564, 690, 725, 814, 945, 966 (hides .~new from dir listing). Enabled by TD src/game/main.c:2918-2919.
- Ours: kernel_file.c:161-280 (NtCreateFile opens saves in place), NtWriteFile :321, NtClose :357.
- Fixes: a console title writes in place (open SUPERSEDE, truncate, write, close). On PC a kill, power loss or full disk between truncate and last write leaves a right-named half file. The guard gives the title a copy; on last handle close a worker flushes and renames over the old name; skips the replace when bytes are identical; keeps `.~pend` if the rename fails and retries at next close/exit/next start; waits up to 2 s at exit; has fault-injection steps for tests.
- Ours has the bug? YES. Evidence: kernel_file.c:238 copies a backup, then CreateFileW with CREATE_ALWAYS/TRUNCATE_EXISTING truncates the real file at open; writes go straight to it; NtClose does not flush.
- PD symptom: zero-length or half-written profile/save after crash, kill or alt-F4 during an autosave; profile then shows as damaged (_ERROR_ slot / cannot load). A plausible contributor to the random _ERROR_ profiles if any run ended during a save (pd-game-v2 has many exit_*.txt automated cycles that kill the process). Also closes the reader race: a profile list reading SaveMeta.xbx while another thread rewrites a profile sees the old whole file until the rename.
- Effort: M. Windows-only ~500 lines after stripping POSIX halves. Needs xbox_host_char/HC macros, a quit hook (recomp_exit_hook.h) and the PD title id (from default.xbe certificate). It guards `\Device\Harddisk0\Partition1\UDATA\<id>` and `...\TDATA\<id>` by translating them with xbox_translate_path, so it works with our current game-dir mapping.
- Licence MIT. Caveats (T header): rename-by-handle of a file being written and handles opened relative to a directory handle are not covered; confirm PD uses neither.

### A2 (P1) Map Partition1\UDATA and TDATA to a user-data root; keep saves out of game files
- Source: U src/kernel/kernel_path.c:56-63 (rules to_save=2), rule_match 124-130 (stops at a separator so UDATAx does not match), s_user_in_game 139-147 plus init 514-557 (legacy saves in game dir kept and logged), RECOMP_SAVE_DIR for just these (U :514-535, validated length). T has the same without CACHE (kernel_path.c:56-61).
- Ours: kernel_path.c:56-78. No UDATA/TDATA rule; `Partition1\` goes to game_dir, so PD saves land in `extracted\UDATA` and custom soundtracks in `extracted\TDATA`.
- Problems: re-extract or delete `extracted\` loses saves (install.ps1 says "Never delete extracted\", a workaround for this); robocopy /E over it; AV quarantine takes saves; per-user separation impossible; RECOMP_SAVE_DIR cannot give a clean test profile; a dump manifest (E1) cannot be checked against a tree that holds saves.
- Fix: copy the two rules, rule_match and the legacy fallback (about 60 lines). Existing users keep working because U keeps `extracted\UDATA` if nothing is in the save dir. Also update ours kernel_file.c:211-243 backup block (assumes UDATA under game_dir) and make_soundtrack.py (writes TDATA under extracted).
- Effort: S-M. Licence MIT. Prerequisite for clean E1/E2.

### A3 (P2) Our pre-truncate backup can destroy the good copy
- Ours: kernel_file.c:211-243 (`CopyFileW(win_path, bak, FALSE)` into `<save_dir>_backup\...`, i.e. %LOCALAPPDATA%\xboxrecomp_backup).
- Bugs by reading: (1) FALSE = overwrite, one slot only. After a crash mid-save the next overwrite-open copies the half file over the good backup. An empty source (size 0, second open after truncate) also overwrites it. (2) Every overwrite-open churns it, so it holds "one save ago", not "start of session". (3) Not atomic: a crash during CopyFileW destroys the previous backup. (4) Only truncating dispositions; TDATA not covered.
- Better design: T src/platform/recomp_save_dir.c: recomp_save_backup 701-776 (dated snapshot of UDATA+TDATA at start, skip if identical to newest, keep 5, prune only its own stamped dirs), copy_file_atomic 314-331 (tmp, fsync, replace), history 1210-1320, export/import with staging and "before-import" backup 778-1200. The guard hook (kernel_saveguard.c ~600) runs just before a changed file replaces the old one.
- Cheap fix: skip when source size is 0 or when a backup of that name already exists this session; write to `name.YYYYMMDD-HHMMSS`; keep last 5 per file. Better: lift recomp_save_backup (~120 lines plus helpers).
- Effort: S (cheap) / M (lift). Licence MIT. P2 (P1 if A1 is not taken).

### A4 (P3, P2 if evidence appears) FATX overwrite semantics: deferred truncate on overwrite-open
- Source: U kernel_file.c:118-190 (overwrite_mark/take/settle/truncate_to_zero), 288-322 (deferred open in NtCreateFile), settle calls in Read/Write/Close/QueryInfo/SetInfo (438-447, 461-470, 544+). T has it too.
- Ours: kernel_file.c:161-250 maps straight to CREATE_ALWAYS/TRUNCATE_EXISTING. Ours lacks it, upstream has it.
- Fixes: on FATX an overwrite sets size 0 but a later SetEndOfFile that grows the file brings back the old bytes (no valid-data length). A title that overwrites and only sets EOF relied on that; NTFS zero-fills.
- PD: unknown. Matters only if PD opens with SUPERSEDE/OVERWRITE_IF and then only sets EOF. Verify with a file log of a save cycle (NtSetInformationFile class 20 without NtWriteFile on that handle).
- Effort: S-M (~90 lines). MIT.

### A5 (P3) Rename by handle unimplemented
- Source: U kernel_bridge.c:4866-4912 (bridge unpacks the 16-byte guest struct), U kernel_file.c:887-946 (SetFileInformationByHandle(FileRenameInfo) with MoveFileExW fallback). T copy marked untested on Windows.
- Ours: kernel_bridge.c:3943-3960 passes any class on; kernel_file.c:461-560 has no case 10, so it logs "unhandled class" and returns STATUS_NOT_IMPLEMENTED.
- PD: any XAPI MoveFile or "write temp then rename" save fails. No MoveFile found in our game code; latent. Effort S (~70 lines). MIT.

### A6 (P2) NtCreateFile Information and status codes (own finding; no fork fixes it)
- Ours kernel_file.c:275: `Information = (disposition == FILE_CREATE) ? 2 : 1` for every success (same in U, T, MC, DJ). NT: FILE_OPENED 1, FILE_CREATED 2, FILE_OVERWRITTEN 3, FILE_SUPERSEDED 0, and OPEN_IF/OVERWRITE_IF report created vs opened. XAPI CreateFile derives ERROR_ALREADY_EXISTS from Information == FILE_OPENED; titles use OPEN_ALWAYS plus that flag to decide "first run, create default".
- Error mapping kernel_file.c:262-269: ERROR_FILE_EXISTS (80, what CREATE_NEW returns) maps to STATUS_UNSUCCESSFUL, not STATUS_OBJECT_NAME_COLLISION; ERROR_SHARING_VIOLATION maps to UNSUCCESSFUL (NT 0xC0000043); ERROR_DISK_FULL in NtWriteFile (:352) maps to UNSUCCESSFUL (NT 0xC000007F). Directory branch line 206 ignores CreateDirectoryW's result, so FILE_CREATE of an existing directory succeeds (should collide). IOSB Status on failure is always OBJECT_NAME_NOT_FOUND (:255-258) regardless of the returned code.
- Fix pattern: after CreateFileW with OPEN_ALWAYS/CREATE_ALWAYS success, GetLastError()==ERROR_ALREADY_EXISTS means it existed. About 30 lines.
- PD symptom: wrong "profile exists" decision on first run or after delete; a candidate for flaky profile states. Unverified that PD reads these; confirm via file log of SaveMeta.xbx opens with disposition 3.
- Effort: S. P2.

---

## B. PATH MAPPING

### B1 (P3) Long paths abort instead of failing
- U kernel_path.c:514-535 rejects RECOMP_SAVE_DIR over MAX_PATH-96. Ours (kernel_path.c:295-296, 470-480) reads RECOMP_SAVE_DIR only when the save arg is NULL, converts into MAX_PATH buffers unchecked and builds with swprintf_s (invalid-parameter handler aborts on overflow). A deep install path crashes on first save-path build. DJ enforces <=140 chars at install (setup/engine.py:151-166). GetFullPathNameW is already in ours (kernel_path.c:322-326). Effort S.

### B2 (P3) Fixed guest code page 1252 instead of host CP_ACP
- T kernel.h:542-550 (`XBOX_GUEST_CP 1252u`), used throughout T kernel_path.c/kernel_file.c. Ours: CP_ACP at kernel_file.c (3 sites) and kernel_path.c (3 sites). A non-1252 host locale (932, UTF-8 manifest) reads the same guest bytes as different characters, so a profile folder with accented characters differs between machines. PD is Japanese-origin. Effort S.

### B3 (P3) No '..' containment in any repo
- Grepped all four kernel_path.c: none sanitise `..`. Ours same. Not reachable by normal PD play. For the installer, DJ setup/engine.py:95-104 safe_relative (reject '..', ':', reserved names, trailing dot/space) and TD disc.py:_safe_join are the patterns.

### B4 (P3) Partition1\CACHE
- U kernel_path.c:64-81 redirects the title's own Partition1\CACHE to `<save>/TitleCache`. Ours has no rule: writes would land in extracted\CACHE. Check with RECOMP_PATH_LOG whether PD touches it.

---

## C. DIRECTORY ENUMERATION and STATUS CODES

### C1 (P3) Tiny buffer / truncated name handling in NtQueryDirectoryFile
- Source: T kernel_file.c:100-157 (XBOX_DIRINFO_HEADER via offsetof, Length < header returns STATUS_INFO_LENGTH_MISMATCH, dirinfo_finish: copy what fits and STATUS_BUFFER_OVERFLOW), uses at ~833-850 and 995.
- Ours kernel_file.c:735-840: line 810 `memset(entry, 0, Length)` and field stores happen before any `Length >= header` check; when the name does not fit, it still reports SUCCESS with FileNameLength and Information beyond the buffer; header size uses the null-pointer member trick (UB).
- Edge only; normal listing unaffected. Effort S. MIT.

### C2 Our enumeration is ahead of every fork; remaining suspects for the flaky `_ERROR_` profile (unverified, ranked)
1. A save rewrite racing a profile read (A1).
2. A6 Information/status fidelity.
3. Dir contexts keyed by host handle value: dropped on NtClose (kernel_file.c:361, bridge :788) but not on handles closed by other routes (U added the same concern for overwrite marks, U kernel_file.c:292-312).
- Evidence tip: DJ wraps NtQueryDirectoryFile with a budgeted 400-line [DIR] log of pattern, restart flag and result (DJ tools/xboxrecomp/src/kernel/kernel_file.c ~745-790). Copy it (about 30 lines) and capture one failing run.

### C3 "*.*" mask: T kernel_file.c:113-118 treats it as "*" (for POSIX). FindFirstFileW already does so; nothing to take.
### C4 (P3) U xbox_path_tree (kernel_path.c:330-387) and xbox_host_parent_exists (kernel_file.c:192-216) give a high/low split for missing-file reports. Diagnostics only.

---

## D. CRASH HANDLING AND REPORTING

### D1 (P1) Persisted crash reports, minidump, run-log rotation
- Source: T src/kernel/xbox_crash.c (1574 lines): log tee + ring 178-480, xbox_crash_begin/printf/end 830-885 writing `<save>/crash/<stamp>-<kind>/{report.txt,log.txt,settings.txt}`, guest stack walker 887-1000, guest RAM windows 1020-1110, run-log rotation 617-660 (current + last 3), hang watchdog 1483-1574. Format doc T xbox_crash.h. Host side: TD src/game/main.c:1629-1700 (unhandled_filter: StackWalk64 then MiniDumpWriteDump MiniDumpWithThreadInfo into the report folder; SetUnhandledExceptionFilter at :1693).
- Ours: pd-game-v2/src/main.c:555-1040 has a VEH (AddVectoredExceptionHandler at :1037) that prints registers and guest chain to stderr only. No SetUnhandledExceptionFilter, no MiniDumpWriteDump (grep). play_watched.bat redirects to run_user.out/run_user.err in the build dir, overwritten on every launch, so the failed run's log is gone the next time the player starts the game.
- Gap: a native crash the VEH does not claim (stack overflow, abort from swprintf_s invalid parameter, heap corruption) gives only the Windows crash dialog and no artefact. Hangs: RECOMP_WATCHDOG_SECS is opt-in and prints only.
- Minimal subset: (a) SetUnhandledExceptionFilter + MiniDumpWriteDump into `%LOCALAPPDATA%\xboxrecomp\crash\<stamp>\` (~40 lines from TD main.c:1629-1690), (b) rotate run_user.err/out to logs\run-N instead of overwriting (batch change), (c) later the tee/report. Effort S for a+b, M for c. Licence MIT. P1 for a+b, P2 for c.

### D2 (P2) Bug-report zip with redaction
- Source: T src/kernel/xbox_report.c (1882 lines): redaction of user, home, host names 128-310 (fits your no-identity-leak rule), dependency-free zip+deflate 375-710, system summary 1195-1360, xbox_BugReport 1537-1720 (logs, last 5 crash reports, settings, optional screenshot). TD docs/running.md "Reporting a bug" (line 102) and "Saves, backups and crash reports" (line 34).
- Ours: none; users send logs by hand. Cheaper: lift only the redactor (~200 lines) and zip with PowerShell Compress-Archive from a REPORT.bat. Effort M-L full, S-M cheap. P2.

### D3 (P3) Hang report: T xbox_crash.c:1483-1574 writes a `hang` report with all-thread samples when flips stop for N seconds. Effort S after D1.

---

## E. INSTALLER (ours: install.ps1 266 lines, INSTALL.bat, make_release.ps1, tools/pipeline/pd_pipeline.py)

Keep from ours: pinned and SHA-256-verified xdvdfs download (install.ps1:35-39,108-125), default.xbe allow-list, `.extract_ok` marker, idempotent steps, input-stamp regeneration skip (pd_pipeline.py input_stamp), PREBUILT mode, transcript log, clean-tree check in make_release.ps1:20-21.

### E1 (P1) Verify the whole dump, not only default.xbe
- Ours install.ps1:156-181: default.xbe SHA plus marker is the only proof. A truncated image, disk-full mid-unpack, or AV removing a file later leaves xbe-OK and marker-OK and a game that dies on a missing .xmv/.awd, indistinguishable from a recomp bug.
- Sources: T src/install/disc_install.c/h (1048+133 lines): per-file manifest {path,size,sha256}, hash while copying to `<name>.part`, `install.journal` of completed files, `.complete` marker last, resumable after kill, refuses unknown discs and says what they are, ignores files not in manifest. TD tools/install/disc.py:100-170 is the Python version: probe() without writing, _safe_join, size-match restart, verify_installed. T src/install/xr_sha256.c is the dependency-free hash. DJ scripts/verify-dump.py + config/dump-manifest.json is the cheaper middle (xbe size/hash, required_paths, expected_file_count; weak: count and existence only).
- For PD: generate `config\pd-dump-manifest.json` ({path,size,sha256}) from a known-good extraction and add a verify step using Get-FileHash; exclude UDATA/TDATA until A2 moves saves out (saves under extracted\ would break the manifest). Sizes first (fast), hashes in full verify.
- Effort: M (manifest S, verify S-M; resumable journal skippable because xdvdfs unpack is rerunnable). Licence MIT (T); DJ/TD check. P1 for size+SHA verify; P3 for journal/resume.

### E2 (P2) Staged extract, promote, install lock
- Ours install.ps1:172-179 extracts over an existing `extracted\` ("Saves are kept"); an interrupted run mixes old and new files and only the xbe hash guards it.
- Sources: TD disc.py:117-165 (`.part` then rename, size-match skip); DJ setup/engine.py validate_locations 151-166 (install, data, dump separate and non-nested), install_lock 169-196 (OS-held lock, released on crash), game_lock 197-230; DJ setup/transaction.py (durable journal, owner marker, backups of replaced owned paths, recover() on next start, allow-list, fsync of directories, never deletes player data).
- For PD, after A2: extract to `extracted.new\`, verify (E1), rename over `extracted\` keeping `extracted.old` until success; add an exclusive-share lock file so two INSTALL.bat runs or PLAY during install are refused (~25 lines of PowerShell). Effort M. P2.

### E3 (P2) Hard preflight checks
- Ours only warns when <20 GB free (install.ps1:101-102). DJ engine.py:151-166 refuses paths >140 chars, with line breaks, a drive root, nested or non-directory targets. MC install_windows.bat:22-27 refuses 32-bit Windows. For PD: refuse (not warn) on low space; refuse paths over ~120 chars or non-ASCII (our MAX_PATH buffers abort on overflow, B1). Effort S.

### E4 Anti-pattern: MC install_windows.bat:30-37 downloads `msys2-base-latest.sfx.exe` from a "latest" URL with no hash and runs it. Do not copy; ours is better. MC tools/extract_disc.sh just refuses if default.xbe exists; ours is better.

### E5 (P3) UX
- TD Qt GUI (docs/installer-gui.md, 378 lines) is heavy; not proportionate. Cheap items: Write-Progress from xdvdfs per-file output (S); a probe/dry-run that identifies the disc and says why it is refused (region, Xbox 360 disc, patched xbe) before writing, as TD disc.py:41-60 messages do (ours says only "not a supported revision", install.ps1:196-198) (S).

### E6 (P3) Release and pipeline
- make_release.ps1: add SHA256SUMS.txt for the zip (S). The zip contains play_watched.bat, which writes run_user.err into the install dir (see D1b).
- pd_pipeline.py: deletes gen *.c/*.h before regenerating but leaves the old .pd_stamp; the recomp_dispatch.c existence check covers it, so no bug. Optional: delete the stamp first (1 line).

---

## F. Other observations on ours (no fork fix)
- NtReadFile/NtWriteFile with ByteOffset use OVERLAPPED on a synchronous handle, which moves the file pointer; T restores the pointer on failed or zero-byte transfers (T kernel_file.c:455-475, BO-221) as NT leaves CurrentByteOffset alone. Ours (kernel_file.c:291-310, 335-350) does not. Low probability; S.
- NtClose does not FlushFileBuffers; acceptable once A1 exists.

---

## Ranked summary
1. A1 save guard (T kernel_saveguard.c): P1, M.
2. A2 UDATA/TDATA user-data root with legacy fallback (U kernel_path.c:56-63,124-147,514-557): P1, S-M.
3. D1 crash dump + log rotation (TD main.c:1629-1700, T xbox_crash.c): P1, S.
4. E1 whole-dump verification (T disc_install.c, TD disc.py, DJ verify-dump.py): P1, M.
5. A3 backup overwrite hazard (ours kernel_file.c:238; T recomp_save_dir.c:701): P2, S.
6. A6 NtCreateFile Information/status (own finding): P2, S.
7. E2/E3 staged extract, lock, preflight (DJ engine.py/transaction.py): P2, M.
8. D2 redacting bug-report (T xbox_report.c): P2, M-L.
9. A4 FATX overwrite, A5 rename, C1 dirinfo checks, B2 CP 1252, B1 long paths: P3, S each.

Not verified: that PD uses rename, OPEN_ALWAYS Information, or SetEndOfFile-only overwrites (needs a file log of a profile create/save cycle); licences of the three game-layer repos; nothing compiled.
