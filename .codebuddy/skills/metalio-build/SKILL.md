---
name: metalio-build
description: "Use this skill when working on the Metalio-S31 Kiikoread firmware: porting the kiiko night-mode (dark-body output polarity), 150ms slider throttling, or the exponential brightness curve / brightness-max-ceiling setting to a new release (rc104/rc108/...), building it with the bundled self-contained .idf toolchain, or producing a TF-card OTA update package (Metalio-S31-update.bin). Also covers the full-page-refresh-on-polarity-flip requirement and the 'same version rejected' OTA rule."
---

# Metalio-S31 (Kiikoread) port & build

This skill captures the port of kiiko's night mode + slider throttling onto the
Metalio-S31 firmware and the self-contained build/OTA workflow, so a future release
(rc108, rc112, …) can be reproduced without re-deriving the design.

## When to use
- Porting night mode / brightness / slider changes to a new Metalio firmware version.
- Building the firmware or generating a TF-card OTA package on a fresh machine.
- Debugging "half-screen residue", polarity-flip artifacts, or brightness-track behavior.

## Mental model (read before editing)
Night mode = **output polarity inversion** at the driver, not two themes. The
framebuffer stays light-semantics; only the waveform LUT (to,from) indices are
reversed (to↔15-to, from↔15-from) at present time. Polarity follows the *body page
only*; menus stay light. **Any polarity flip MUST be paired with a full-page refresh**
(differential is computed in logical gray, so old-polarity pixels would diff to 0 and
never repaint). Body images need positive compensation (`ui_image_night_gray`). See
references/porting.md §语义.

## Porting checklist (kiiko → rc104)
The full A–H change list with file anchors and code is in references/porting.md.
Anchors are function names / unique strings — search for them, never rely on line
numbers. Quick map:

| Area | Files | Anchors |
|------|-------|---------|
| A driver polarity | components/epdiy/src/epdiy.h, render_context.h, boards/metalio47+pico render_context.c, boards/metalio47 render.c | epd_set_inverted, epd_polarity_phases, s_first_active_phases |
| B settings | main/settings.c, settings.h | NVS_KEY_NIGHT, app_settings_night_mode |
| C polarity follow + full refresh | main/app/app_loop.c | apply_output_polarity, s_last_body_dark (7 sites) |
| D lock/sleep | boards/metalio47/src/main/sleep.c, main/apps/app_sleep.c | epd_set_inverted(false) |
| E app_book | main/apps/app_book.c | BOOK_TOOL_COUNT 6, draw_night_icon, reader_slider_feedback, ui_image_night_gray |
| F ui_kit | main/ui/ui_kit.c, ui_kit.h | ui_image_night_gray |
| G brightness exp curve + max ceiling | main/ui/ui_quick_brightness.h, settings.c/h, app_device_settings.c | UI_QUICK_BRIGHTNESS_STEP, app_settings_brightness_max, SETTINGS_FRONTLIGHT |
| H build/OTA | build_metalio.py | --ver, --sd |

Follow the numbered checklist in references/porting.md §rc108 移植检查单.

## Building (self-contained .idf toolchain)
All details in references/build.md. Summary:
- ESP-IDF v6.1, the RISC-V toolchain, cmake/ninja, and the python venv are bundled
  under `.idf/` (git-ignored). Copy the whole project dir to another machine to
  build — no dependency on `C:\Users\qa`.
- Build: `python build_metalio.py --ver <ver> --sd` (uses the .idf venv automatically).
- TF-card OTA rejects a package whose version equals the running one, so **always bump
  --ver** (rc104 → n1 → n2 …). Output: `Metalio-S31-update.bin` at project root.
- After moving the project dir or changing toolchain paths, delete `build/` and rebuild.

## Pitfalls
- Polarity flip without full refresh → half-screen residue. (C-2, C-3)
- render.c first-active phase cache must be dropped when polarity is on (A5).
- Brightness value < 1 collapses to 0; position 0 returns 0 (logf(0) UB) (G).
- OTA same-version rejected → bump --ver (H).
- On a fresh machine, if the python install path differs, fix `home`/`executable` in
  `.idf/python_env/idf6.1_py3.13_env/pyvenv.cfg`.
- Constraints file is named `espidf.constraints.v6.1.txt` (no hyphen) under `.idf/`.
