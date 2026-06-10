# Manual Sports Logo Cleanup

Edit the PNGs in this folder. Do not edit files in `sports_logos/generated/masks`
or `SportsLogosGenerated.cpp`; those regenerate automatically.

## Files to Edit

- `nhl/ANA.png`: simplify to a bolder duck/letter mark.
- `nhl/FLA.png`: simplify shield/panther details.
- `nhl/LA.png`: use a higher-contrast crown or LA mark.
- `nhl/NYR.png`: simplify shield/text into bold diagonal colors or a letter mark.
- `nhl/OTT.png`: reduce face/helmet detail.
- `nfl/LAR.png`: thicken horn and letter strokes.
- `nfl/NYJ.png`: replace thin wordmark with big NY or simplified oval.
- `nba/BKN.png`: clean ring and B mark.
- `nba/DAL.png`: simplify horse/ball detail.
- `nba/IND.png`: clean thin internal letter/ball shapes.
- `nba/MEM.png`: simplify bear face to stronger eyes/muzzle.
- `nba/MIN.png`: simplify wolf/ball detail.
- `nba/NO.png`: simplify to a bird/fleur-de-lis style mark.
- `nba/WSH.png`: simplify roundel/text to monument/ball mark.
- `mlb/ARI.png`: simplify to a stronger A mark.
- `mlb/ATH.png`: simplify to an A mark or clean elephant details.
- `mlb/BAL.png`: reduce bird head detail and strengthen outline.
- `mlb/BOS.png`: harden socks edges.
- `mlb/CHC.png`: use a bold C, not roundel text.
- `mlb/CLE.png`: thicken script C edges.
- `mlb/COL.png`: simplify CR monogram.
- `mlb/HOU.png`: clean star/roundel small-color noise.
- `mlb/SEA.png`: simplify compass details.
- `mlb/TOR.png`: simplify blue jay and maple leaf details.
- `mlb/WSH.png`: harden script W edges.

## Photoshop Settings

- Open the PNG directly from this folder.
- Zoom to `1600%` or higher.
- Use the Pencil tool, `1px`, no anti-aliasing.
- Keep the canvas `32x32`.
- Keep transparency around the logo.
- Prefer `2-5` solid colors.
- Make important strokes at least `2px` thick.
- Delete tiny text, small holes, and 1px noise that will not read on LEDs.

## Regenerate

After saving edits, run from the repo root:

```sh
python3 tools/generate_sports_logos.py
```

Then inspect:

- `sports_logos/manual/contact_sheet.png` for the manual originals you edited.
- `sports_logos/generated/contact_sheet.png` for the final quantized output.

When a logo looks good, change its entry in `sports_logos/review_status.json`
from `"status": "manual"` to `"status": "pass"`. Leave the `sourceOverride`
in place so the generator continues using your cleaned PNG.
