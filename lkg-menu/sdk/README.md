# OpenGlass Go External App SDK

C11 SDK for building standalone apps launched by `lkg-menu` from `/home/lkg/data/apps/<app>/`.

## Package Layout on the SD Card (`LKG_DATA`)

```text
apps/
  my-app/
    app.json
    my-app          # ARM64 Linux executable (can be 0644 on exFAT)
```

### `app.json`

```json
{
  "name": "My App",
  "exec": "my-app",
  "version": "1.0.0",
  "description": "Short summary",
  "author": "Author",
  "render": "hosted",
  "api_version": 1
}
```

- `name` (required): Display name shown in the `Apps` menu.
- `exec` (required): Relative executable path inside the app folder (no `/` prefix or `..`).
- `render` (optional): `"hosted"` (default) or `"drm"`.
- `api_version` (optional): Protocol version, currently `1`.

## Environment Variables Passed to the App

- `LKG_APP_FD`: Socket file descriptor used automatically by `lkg_app_open()`.
- `LKG_APP_DIR`: Path to the app folder (also set as the working directory).
- `LKG_APP_NAME`: Display name from `app.json`.
- `LKG_DATA_DIR`: Path to `/home/lkg/data`.
- `LKG_CALIBRATION`: Path to `/run/lkg/visual.json`.

## Hosted Mode (`"render": "hosted"`)

Apps link only against `libc` and `sdk/lkg_app.c` (`-std=c11 -I<path-to-sdk>`):

- **Constants & Theme**: `lkg_app_info(app)` exposes `screen_width`, `screen_height`, `row_height` (`vdisplay / 17`), `margin` (`hdisplay / 12`), `top_margin`, `bar_gap` (`8`), `menu_3d`, `button_reversed`, standard font sizes (`LKG_FONT_SIZE_LARGE` 64, `LKG_FONT_SIZE_KEY` 56, `LKG_FONT_SIZE_MEDIUM` 48, `LKG_FONT_SIZE_CAPTION` 43, `LKG_FONT_SIZE_SMALL` 32), layer depths (`LKG_DEPTH_SUBTLE_POP_OUT` +35.0f / `LKG_DEPTH_SUBTLE_RECESSED` -28.0f as default `LKG_DEPTH_POP_OUT`/`LKG_DEPTH_RECESSED`, `LKG_DEPTH_DEEP_POP_OUT` +70.0f / `LKG_DEPTH_DEEP_RECESSED` -56.0f, `LKG_DEPTH_SCREEN` 0.0f), and standard ARGB colors.
- **Input**: `lkg_app_poll_event()` returns `LKG_EVENT_BUTTON` (normalized physical buttons `LKG_NAV_PREV`/`NEXT`/`CONFIRM`/`BACK`), `LKG_EVENT_KEY` (USB keyboard arrows, Enter, Esc, Backspace, and translated characters plus raw Linux `keycode`), and `LKG_EVENT_QUIT`.
- **Virtual Keyboard**: `lkg_app_text_input()` opens the menu's 3D on-screen keyboard modal (with USB keyboard passthrough) and returns up to 64 characters.
- **3D Layered Rendering**: Draw onto `LKG_LAYER_FRONT` (0) and `LKG_LAYER_BACK` (1) using `lkg_app_draw_rect`, `lkg_app_draw_text`, `lkg_app_draw_text_fit`, `lkg_app_draw_back_arrow`, `lkg_app_draw_menu_row`, or write ARGB8888 pixels directly into the shared memory buffer from `lkg_app_layer_pixels()`, then call `lkg_app_present*()` to interlace on GPU and flip DRM.

## DRM Mode (`"render": "drm"`)

When `"render": "drm"` is set, `lkg-menu` suspends its GPU/layers and drops DRM master before launching the app so the app can claim `/dev/dri/card0` directly while still receiving `LKG_EVENT_*` input and constants via `lkg_app_open()` / `lkg_app_poll_event()`.
