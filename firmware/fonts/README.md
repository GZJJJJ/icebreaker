# Icebreaker Text 16

16-pixel bitmap subset derived from **GNU Unifont 16.0.04**, distributed here
under the upstream **SIL Open Font License 1.1** option (see `OFL-1.1.txt`).
The modified font is named Icebreaker Text 16, not Unifont.

Upstream copyright (from the 16.0.04 BDF header):

Copyright (C) 1998-2025 Roman Czyborra, Paul Hardy, Qianqian Fang,
Andrew Miller, Johnnie Weaver, David Corbett, Nils Moskopp,
Rebecca Bettencourt, Minseo Lee, Ho-Seok Ee, et al.

Source: <https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz>

Upstream compressed source SHA256:
`f9c8c7802453f47be02677176aeac2342ee96d354fad7a26cedcce48e68e1d9f`

- `icebreaker_text16.hex.gz`: editable subset source, one `CODE:BITMAP` per line.
- `icebreaker_text16.bin`: generated 688160-byte Flash asset, 16 rows of two
  big-endian bytes per glyph. Original 8-pixel-wide rows occupy the high byte.
- Ranges, in order: U+0020..007E, U+00B7, U+2000..206F, U+25A1, U+3000..303F,
  U+4E00..9FFF, U+FF00..FFEF. Unsupported characters display U+25A1.

Regenerate from the upstream source or this subset source:

```powershell
python tools/build_display_font.py <source.hex.gz>
```

The firmware build embeds the checked-in binary; it does not download or
rasterize fonts. Changes from upstream: subset selection, renaming, and packing
each glyph into 32 bytes for direct Flash lookup. No glyph shapes were redrawn.
