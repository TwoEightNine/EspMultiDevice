# UI Kit contract

`src/ui/UiKit.h/.cpp` and `UiText.h/.cpp` are the mandatory shared drawing contract for application screens. Do not copy primitive app-bar, icon, button, list-row, or progress-bar drawing into a screen. Home, Splash, and the full-screen Image Viewer are intentional layout exceptions; the viewer still uses the shared close affordance.

## App bars and navigation

Clear the screen, then draw the bar before screen content:

```cpp
UiKit::drawTopAppBar(_display.tft(), _display.width(), F("My screen"));
```

The helper draws a 48px `#111` bar. Its 24×24 antialiased navigation glyph has 12px padding inside a 48×48 leading hit target. The title begins at x=48, sits 2px high for visual centering, and ellipsizes before the 16px trailing margin. Close is standard; use Back only for in-screen history.

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

Use `UiKit`’s 24px navigation/control icons and its 16px list/keyboard icons. Every icon function takes its foreground and the immediate background color. Pass the true background color so TFT_eSPI antialiasing blends without display readback.

For a cross-screen shape, add one documented `UiKit` function and use it everywhere. Build it with TFT_eSPI smooth primitives such as `drawWideLine`, `fillSmoothCircle`, `drawSmoothCircle`, `fillSmoothRoundRect`, or `drawSmoothRoundRect`. Do not use pixelized `drawLine`, `fillCircle`, or `drawRoundRect` for UI icons and rounded controls.

For full-colour compile-time icons, generate straight RGB565 plus 8-bit alpha
arrays with `tools/png_to_rgba565_icon.py` and draw them through
`UiKit::drawRgba565Icon()`. Pass the icon's actual solid immediate background
colour; the helper alpha-blends every edge pixel without an SD card or display
readback. Its maximum width is `UiKit::MaxRgba565IconWidth` (64 px).

Use `UiText` for all application text:

- `Compact`: metadata.
- `Content`: normal content and button labels.
- `Title`: app-bar titles and deliberate emphasis.

`UiText` supplies smooth FreeSans faces covering Latin and Cyrillic. Redrawing text or an antialiased icon requires the same immediate background color used on its first draw.

## Buttons, inputs, and progress

Rounded controls are fill-only smooth rounded rectangles: use `fillSmoothRoundRect()` with the fill as the edge background, never an outline. Labels default to `UiText::Size::Content`; numeric PIN keys are the sole exception and use `Title`.

Input fields are smooth rounded rectangles whose interiors are cleared without overwriting the antialiased outline. Leave at least 6px below the app bar. PIN labels are `Enter PIN` and entered digits are 10px antialiased circles, not asterisks.

Use `UiKit::drawProgressBar()` and `updateProgressBar()` for progress. For icon-only footer controls, the entire visual cell is the target. Active controls use active fill/foreground; inactive controls retain the ordinary footer background and a gray icon—do not invent an inactive fill.
