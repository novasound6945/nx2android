# qrcodegen

Project Nayuki's QR Code generator library, its C version: `qrcodegen.c` and `qrcodegen.h`, unmodified.

- Source: https://github.com/nayuki/QR-Code-generator, tag `v1.8.0` (commit `720f62bddb7226106071d4728c292cb1df519ceb`), the files under `c/`
- SHA-256 of the files as taken (2026-11-21):
  - `qrcodegen.c` `300eff07ee25baaa7578f20284411638154716379437391e7e689c0e6ce81403`
  - `qrcodegen.h` `e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589`
- License: MIT (`LICENSE`, the text at the top of both files)

Used by the Android start screen's device link (`dm/android/startscreen.c`, which includes `qrcodegen.c`; its symbols stay hidden in the library): the QR code that opens rooms' link page
with the code filled in. Check a new version with: `curl -fsSL https://raw.githubusercontent.com/nayuki/QR-Code-generator/<tag>/c/qrcodegen.c | shasum -a 256`.
