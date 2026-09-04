# UI implementation requirements

Before adding or changing a screen, read the root [`README.md`](README.md), especially **UI Kit**. Its UI contract is mandatory.

- Use `UiKit` for app bars, navigation hit testing, shared icons, list cells, and shared progress controls. Add a reusable component to `UiKit` instead of duplicating drawing primitives in a screen.
- Use TFT_eSPI antialiased primitives for icons, circles, rounded rectangles, and button fills. Supply the immediate background color to every smooth primitive.
- Use a 48px app bar with the kit's 24px icon in its 48×48px hit target; do not hand-position a replacement icon or title.
- Keep icon-only interactive targets equal to their whole visual cells. List actions are 40×40px with 16×16px kit icons and 12px padding.
- Use fill-only smooth rounded buttons and `UiText::Size::Content` labels unless the README documents a screen-specific exception.
