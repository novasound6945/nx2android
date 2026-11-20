# Noto Sans KR (the start screen's font)

The Android start screen's text (`dm/android/startscreen.c`: the data download, the shader preparation and the cloud save's start, before the game) is
[Noto Sans KR](https://fonts.google.com/noto/specimen/Noto+Sans+KR), as Google Fonts publishes it in [google/fonts](https://github.com/google/fonts/tree/main/ofl/notosanskr):
`NotoSansKR[wght].ttf` (the variable font, SHA-256 `194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252`) and its `OFL.txt`
(`1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9`), taken 2026-11-20.

The two files here are that font cut to what the screen draws, made with

```
python nxnative/dm/tools/make_start_font.py 'NotoSansKR[wght].ttf' nxnative/dm/third_party/noto-sans-kr
```

an instance at weight 400 (`NotoSansKR-Regular.subset.ttf`) and at 700 (`NotoSansKR-Bold.subset.ttf`), each with the 2,350 Hangul syllables of KS X 1001 (a game's
name in Korean), the compatibility jamo, ASCII, Latin-1 and the punctuation and arrows the screens use; no hinting, no OpenType layout tables. A code point outside it
(a name in Japanese) is drawn with the overlay's pixel font (`../fusion-pixel-font`).

The font is under the SIL Open Font License 1.1 (`OFL.txt`). Its Reserved Font Name is "Source", which these files do not use: their names stay Noto Sans KR. Every APK
carries the license beside the fonts, as `assets/font/LICENSES/noto-sans-kr/OFL.txt` (porter/pipeline.py `ui_font_assets`). The glyphs are drawn with
[stb_truetype](https://github.com/nothings/stb) 1.28 (`../stb/stb_truetype.h`, public domain or MIT).
