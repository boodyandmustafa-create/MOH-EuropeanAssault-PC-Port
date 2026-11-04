# POSIX directory-search lifetime

A game-free fixture for the real POSIX `NtCreateFile`, `NtQueryDirectoryFile`,
`NtClose`, path translation and host handle implementations. It builds only
file/path and platform sources, so it runs against upstream main without the
POSIX guest-memory/renderer prerequisites required by the full runtime.
Logging and the unused symbolic-link registry are stand-ins; directory paths,
searches and handle allocation are not mocked.

```sh
cmake -S tests/kernel_directory_posix -B build/kernel-directory-posix \
  -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/kernel-directory-posix
ctest --test-dir build/kernel-directory-posix --output-on-failure
```

For an AddressSanitizer build, add `-DXBOX_DIRECTORY_ASAN=ON` to a separate
configure directory. ASan's allocation quarantine is useful here: without
immediate handle reuse, an unfixed backend exhausts its 64-context table.
The fixture uses explicit checks, so Release/NDEBUG cannot remove validation.

Checks:

- 300 wildcard searches across eight directories, each closed after its first
  result with `RestartScan=0` on the next search; every result must belong to
  the newly requested directory, irrespective of filesystem listing order.
- Stable open-descriptor count after the abandoned searches.
- Closing one live search preserves another search and its restart behavior.
- Closing an unsearched directory or ordinary file succeeds; null close keeps
  the existing invalid-handle status.

The test creates and removes its own temporary directories and synthetic files.
It needs no title binary, game data or saves. It deliberately rejects Windows;
`tests/kernel_directory` covers that backend separately. It does not test
concurrent queries and close on the same handle, or change their API contract.
