# macOS release dependencies

The release prefix targets **macOS 14.4 / arm64** and isolates application
libraries from Homebrew. Homebrew may still provide build tools.

From the repository root:

```sh
python3 scripts/macos-deps/prepare.py
python3 scripts/macos-deps/verify.py "$PWD/cmake-deps/macos/arm64"
export CANVASHOST_MACOS_DEPS="$PWD/cmake-deps/macos/arm64"
cmake -S . -B cmake-build-production-isolated -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/macos-release.cmake" \
  -DFFMPEG_PREPARED_BINARIES="$CANVASHOST_MACOS_DEPS/ffmpeg" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF
cmake --build cmake-build-production-isolated --target lumen --parallel 6
```

`sources.json` locks source archives and Qt security patches by SHA-256. Qt uses
bundled third-party libraries; GLib, D-Bus, ICU and networking are disabled.
Boost.Locale uses system iconv and macOS supplies libcurl. FFmpeg retains the
repository's VideoToolbox patches, including HEVC 4:4:4.

Generated files live under ignored `cmake-deps/macos/`. The installed prefix
contains absolute paths and is not relocatable. Move the work directory aside
and rebuild after changing the repository location, dependency recipe or SDK.
`manifest.json` records sources, compiler, SDK and installed file checksums.
Completed steps resume after interruption; subsequent preparation verifies and
reuses the completed prefix.

Installation/CPack checks every bundled Mach-O before signing for architecture,
minimum OS, external dependencies and the known incompatible `pipe2` import.
Old-system launch and streaming tests remain necessary.

```sh
python3 scripts/macos-deps/audit.py /path/to/Sunshine.app --target 14.4 --arch arm64
python3 -m unittest discover -s scripts/macos-deps -p 'test_*.py'
```
