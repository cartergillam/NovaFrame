# Sports Logo Pipeline

Source logos live in `sports_logos/source/{league}/{TEAM}.png`.

Generate 32x32 normalized previews, quantized previews, color masks, a contact sheet,
and C++ bitmap arrays:

```sh
python3 tools/generate_sports_logos.py
```

The generator reads `sports_logos/review_status.json` by default. Each logo must be
classified as:

- `pass`: recognizable enough for firmware.
- `manual`: needs Photoshop/source-logo simplification before firmware use.
- `tune`: unresolved generator tuning item.

Useful tuning options:

```sh
python3 tools/generate_sports_logos.py --max-colors 4
python3 tools/generate_sports_logos.py --padding 2
python3 tools/generate_sports_logos.py --alpha-threshold 80
python3 tools/generate_sports_logos.py --review-manifest sports_logos/review_status.json
```

Outputs:

- `sports_logos/generated/contact_sheet.png`: first-pass visual QA.
- `sports_logos/generated/normalized/{league}/{TEAM}.png`: resized transparent 32x32 logo.
- `sports_logos/generated/quantized/{league}/{TEAM}.png`: clustered 32x32 preview.
- `sports_logos/generated/masks/{league}/{TEAM}/layer_*.png`: one 1-bit color mask per layer.
- `SportsLogosGenerated.h`: firmware header in the sketch root.
- `SportsLogosGenerated.cpp`: firmware bitmap arrays in the sketch root.

Manual cleanup workflow:

1. Find bad logos in `contact_sheet.png`.
2. Check the matching entry in `review_status.json`.
3. Edit the source PNG or add an `overrides.sourceOverride` path for a cleaned logo.
4. Add per-logo overrides if needed: `maxColors`, `padding`, or `alphaThreshold`.
5. Re-run the generator.

The generated C++ is not wired into `SportsApp` yet. It is ready to be wrapped by
a `drawSportsLogo(league, teamId, x, y)` helper.
