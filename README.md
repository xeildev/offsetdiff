# offsetdiff

A lightweight Windows utility for comparing offset dumps.

## Features

- Compare two nested JSON offset dumps by full key path
- Detect added, removed, changed, and unchanged offsets
- Handle 64-bit integers and hexadecimal values without precision loss
- Search, filter, and sort results
- Copy names and values; export comparisons to JSON or TXT
- Load files through the file picker or drag and drop

## Usage

1. Launch `offsetdiff.exe` on Windows 10/11 x64.
2. Select the old and new JSON files. The comparison runs automatically.
3. Review the results. Click a status card to filter or a column heading to sort.
4. Double-click a cell to copy it, or right-click a row for copy options.
5. Click **Export** to save the full comparison as JSON or TXT.

Exports include all rows, regardless of the current search or filter.
Input files are never modified. Dropping two files assigns them to OLD and NEW
in the order supplied by File Explorer; check the displayed filenames.

Input must be a UTF-8 JSON object containing integers, hexadecimal strings,
or nested objects. For example:

```json
{"client": {"dwEntityList": "0x123456", "m_iHealth": 820}}
```

Values range from INT64_MIN to UINT64_MAX. Fractions, exponent notation,
arrays, booleans, nulls, and duplicate keys are rejected. The limit is 512 MiB
per file and 256 object levels; memory use can exceed the input size.
Dots and backslashes in keys are escaped in displayed paths; empty keys use
`\e`. Differences are exact signed hexadecimal values: NEW minus OLD.
Identical offsets means matching parsed names and values, regardless of formatting.

Keyboard shortcuts: `Ctrl+O` selects OLD, `Ctrl+Shift+O` selects NEW, `Ctrl+F`
focuses search, and `Ctrl+E` opens export. With the
table focused, use arrow keys to select cells, `Ctrl+C` to copy, and `F5` to reload.
Press `Esc` in the search field to clear it.

Window size, search, filter, and sort preferences are saved in
`%LOCALAPPDATA%\xeil\offsetdiff\settings.json`.

## Building

Install CMake 3.24+ and Visual Studio 2022 or Build Tools 2022 with the
**Desktop development with C++** workload (MSVC v143 and a Windows 10/11 SDK).
Run these commands in PowerShell from the project directory:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/xeil-offsetdiff-0.1.0
.\dist\xeil-offsetdiff-0.1.0\offsetdiff.exe
```

Alternatively, `build-release.ps1` builds, tests, and packages the portable
release under `dist/`. It preserves the previous release ZIP under `dist/previous/`.
The MSVC runtime is linked statically; no separate runtime installation is needed.
The application does not require administrator rights or a network connection.

The C++20 application uses Win32, Direct2D, and DirectWrite. Its only external
dependency, nlohmann/json 3.11.3, is included in `third_party/`; builds work offline.
To build just the comparison engine and tests, configure a separate build directory
with `-DOFFSETDIFF_BUILD_GUI=OFF`.

## License

MIT. See [LICENSE](LICENSE). The nlohmann/json license is included in
[third_party/LICENSE.nlohmann](third_party/LICENSE.nlohmann).
