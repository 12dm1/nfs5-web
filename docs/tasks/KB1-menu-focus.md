# KB1: menu focus for keyboard and gamepad (experimental)

Status 2026-11-23: opt-in with `?focus=1` (`/game?focus=1` on the public site). Without the flag the input path is
the same as before. Research and code 2026-11-05; merged into main 2026-11-23. Addresses: Porsche Unleashed v3.5,
base `0x00400000`.

## What it does

`web/src/focus-page.ts` reads the game's live widget tree from guest memory every frame, `web/src/focus-geometry.ts`
picks the next item by direction, and `web/src/input.ts` draws a yellow frame over the focused item (cyan while a value
is being changed) and drives the game with ordinary mouse messages: move to the item, then button down and up. The
game's own code handles every click, so its checks (disabled items, prices, confirmations) stay in force.

- Arrows / d-pad move the focus, Tab walks in order, Enter / pad A activates, Esc / pad B goes back.
- A real mouse move drops the focus; the gamepad cursor (GP1) still works, the left stick drops the focus too.
- Esc presses the page's `GOBACK`, `GOBACKMINUSONE` or `CANCELCREATEPLAYER` button when one is there.
- Race pause menu: Continue, the quit dialog and Esc back out are covered.

Control types covered (vtable -> kind): `FEGoToButton 0x005B5D10`, bottom bar buttons `0x005B5D80`, `FEInputBox`,
`FEComboBox 0x005B5C24`, `ColorBox 0x005B5BA8`, `FEColorSelectBox`, `FEListBox 0x005B611C`, sliders with an integer
accessor (`0x005B5E60`), key binding (`0x005BAC84`), graphic tabs; checkbox, radio, tab, camera select, chat/IP field
and car setting buttons are recognised from the RTTI but not checked in the game.

## Game structures used

- FE manager `[0x0065B928]`; `[FE+0xD8]` = vector of layers (layer `+0` current window, `+4` pending); `[FE+0xDC]` =
  the shared bottom bar window. Only the top layer is used: the layer below also has live objects.
- Window: constructor `0x00527F90` creates the child pointer vector at `[window+0x10C]`; hit test `0x00528C00`
  subtracts the window position (`0x00525310`) and asks each child's `vtbl+0x4C` in vector order.
- Object position: `[object+0xE0]` attribute, `+0x38/+0x3C` (bound to `pos` in `0x005246E0`); size `+0x100/+0x104`.
- `FEControl+0x10C`: bit `0x10` = disabled (checked in `0x004C7E40`), bit `0x40` = combo expanded. The window's
  `+0x10C` vector offset must not be applied to arbitrary controls.
- Expanded `FEComboBox`: `+0x1A4` row under the mouse, `+0x1A8` its index, `+0x1C4` height of the open area, `+0x1B8`
  scroll bar (vtable `0x005B6A64`), its `+0x11C/+0x12C` = down/up arrow rectangles. Row step 16 px (observed on the
  track list only). At the edge of the visible rows the adapter clicks the arrow and returns the cursor.
- Custom colour editor: flag `0x0065B710`; hit rects `+0x224, +0x234, +0x274, +0x254, +0x264`; HSV `+0x288..+0x290`.
- Game mode for the input (`pu_pad_mode()` in `runtime/pu_ext.c`): mailbox `+7728`, 0 frontend, 1 race menu, 2 driving.
  Focus is off while driving.

## Checked (headless Chrome, 2026-11-05)

- selectname: Create / Remove / Continue flags `0 / 0x10 / 0x14` with an empty list (disabled bit as above).
- Esc on createplayer returned to selectname; Esc on Quick Race returned to gamesetup.
- Quick Race: arrow + Enter opened the track list, Down set hover row 1, Enter closed it; scroll index 0 -> 3 -> 0 with
  Down/Up series; Esc keeps the current index. The colour list: Down + Enter picked row 1.
- Custom colour: a value went 0.5 -> 0.5467, cancel and confirm closed the editor.
- Sound slider 127 -> 126 with Enter + Left; key binding: Tab/Enter then X changed the DirectInput code 80 -> 45.
- Race pause: Continue, quit dialog back, Esc out of the pause.
- Options tabs: Tab + Enter switched to the controls tab (10 binding items visible after).
- `node web/tools/focus_test.mjs`: the snapshot reader and the input routing on fake guest memory (passes 2026-11-23).

## Not done

- Rotating the 3D car (the mouse-zone attempt did not rotate it; adapter removed).
- Text entry from a gamepad alone, binding gamepad buttons, replay save / overwrite pages (recognised only statically).
- Other list types and font sizes for the 16 px row step; a real gamepad; colour persistence across pages.
- Not every page of the inventory (career, garage, parts, tuning, paint shop, network lobby) was walked.
