# UI Kit contract

`src/ui/UiKit.h/.cpp` and `UiText.h/.cpp` are the mandatory shared drawing contract for application screens. Do not copy primitive app-bar, icon, button, list-row, or progress-bar drawing into a screen. Home, Splash, and the full-screen Image Viewer are intentional layout exceptions; the viewer still uses the shared close affordance.

## App bars and navigation

Clear the screen, then draw the bar before screen content:

```cpp
UiKit::drawTopAppBar(_display.tft(), _display.width(), F("My screen"));
```

The helper draws a 48px `#111` bar. Its close glyph uses the generated 40×40 alpha asset, centered inside the 48×48 leading hit target. The title begins at x=48, sits 2px high for visual centering, and ellipsizes before the 16px trailing margin. Close is standard; use Back only for in-screen history.

Reserve y=0–47. Make the entire 48×48 cell tappable:

```cpp
if (UiKit::navigationIconContains(point.x, point.y)) {
  _navigator.pop();
  return;
}
```

## List cells and action targets

Use `UiKit::drawListCell()` for standard rows. A row is 40px tall; labels use font 2 and start at x=16 without an icon or x=56 with the optional scaled 32px icon. Dividers begin at the label x-position and end 16px before the edge. Optional trailing text is compact and right aligned.

The helper only draws; the owning screen must make every x coordinate across the row and the entire 40px height tappable. Derive layout from `UiKit::ListCellHeight`, not local duplicated constants.

An icon-only list action has a 40×40 hit cell, a 16×16 kit glyph, and 12px padding. Do not limit its hit test to the visible glyph.

## Icons, smooth primitives, and text

Use `UiKit`’s generated 40px footer controls and 16px list/keyboard icons. The keyboard assets include off, one-shot Shift, and Caps Lock states. Every icon function takes its foreground and the immediate background color. Pass the true background color so TFT_eSPI antialiasing blends without display readback.

For a cross-screen shape, add one documented `UiKit` function and use it everywhere. Build it with TFT_eSPI smooth primitives such as `drawWideLine`, `fillSmoothCircle`, `drawSmoothCircle`, `fillSmoothRoundRect`, or `drawSmoothRoundRect`. Do not use pixelized `drawLine`, `fillCircle`, or `drawRoundRect` for UI icons and rounded controls.

For full-colour compile-time icons, generate straight RGB565 plus 8-bit alpha
arrays with `tools/png_to_rgba565_icon.py` and draw them through
`UiKit::drawRgba565Icon()`. Pass the icon's actual solid immediate background
colour; the helper alpha-blends every edge pixel without an SD card or display
readback. Its maximum width is `UiKit::MaxRgba565IconWidth` (64 px).

For a monochrome icon that needs runtime tinting, use its 8-bit alpha array
with `UiKit::drawAlphaIcon()` and pass the desired foreground and immediate
background colours. This retains antialiased edges without retaining a colour
array at draw time. Both compile-time icon helpers leave fully transparent
pixels untouched, so a non-rectangular parent background remains visible;
redraw that background before replacing an existing icon.

The 32px Files folder and document assets use `drawListCell()`'s alpha-icon
variant so directory and media-type tints continue to apply. Browser uses the
generated 40px close, back, home, recent, favorite, page-up, and page-down
assets in footer controls; its saved-site removal control uses the 16px close
asset.

Use `UiText` for all application text:

- `Compact`: metadata.
- `Content`: normal content and button labels.
- `Title`: app-bar titles and deliberate emphasis.

`UiText` supplies smooth FreeSans faces covering Latin and Cyrillic. Redrawing text or an antialiased icon requires the same immediate background color used on its first draw.

## Buttons, inputs, and progress

Rounded controls are fill-only smooth rounded rectangles: use `fillSmoothRoundRect()` with the fill as the edge background, never an outline. Labels default to `UiText::Size::Content`; numeric PIN keys are the sole exception and use `Title`.

Input fields are smooth rounded rectangles whose interiors are cleared without overwriting the antialiased outline. Leave at least 6px below the app bar. PIN labels are `Enter PIN` and entered digits are 10px antialiased circles, not asterisks.

Use `UiKit::drawProgressBar()` and `updateProgressBar()` for progress. For icon-only footer controls, the entire visual cell is the target. Active controls use active fill/foreground; inactive controls retain the ordinary footer background and a gray icon—do not invent an inactive fill.
