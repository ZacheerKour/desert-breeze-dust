# Save guard regression

Asset-free Windows x64 test of the save guard in `src/kernel/kernel_file.c`. A save opened for writing goes to `<name>.~new`; the close flushes and renames it over the old file, so the old save stays whole until then. It checks truncating and in-place rewrites, dispositions left to the real file, the start-up sweep (stray `.~new` dropped, finished `.~pend` applied), the once-per-session dated backup and the pruning to five generations.

```powershell
cmake -S tests/kernel_save_guard -B build/kernel-save-guard -A x64
cmake --build build/kernel-save-guard --config Release
ctest --test-dir build/kernel-save-guard -C Release --output-on-failure
```
