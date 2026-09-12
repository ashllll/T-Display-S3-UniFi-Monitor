# UniFi Network console redesign

Reference: the signed-in UniFi Network 10.6.101 dashboard inspected 2026-09-12.
The actual console uses a dark application shell, a slim icon rail, a Network
header, device silhouettes, compact inventory rows, and time-based activity.

This implementation replaces the earlier ten-page meter layout with five views:
Overview, Devices, Clients, Traffic and Console. It removes the large-number
focus page, mirrored speed pages, dot pagination, and continuously scrolling
pixel history. Navigation now selects a persistent application section.

Colors adapted from the reference: canvas #111315, surface #191C1F, header
#25282C, border #30343A, text #EBEDF0, muted #9299A3, blue #579AFF,
purple #AC8CEB, green #73B880. Fonts are open-source Lato 12 and 20px,
the fallback in UniFi's website font stack. These are small-screen adaptation
values, not official published tokens. Device silhouettes are local LVGL shapes.

The preview is compiled from the actual `src/unifi_view.c` using LVGL 9.2.2.
`preview.png` shows all five views and the no-data state; `states.png` shows
startup, 100% system load and expired-data fixtures. Values and client names
in previews are synthetic. Authenticated API checks and physical screen
acceptance are distinct: host previews do not replace physical screen acceptance.

Regenerate with `python3 tests/render_unifi_ui.py` (PPM files in
`/tmp/unifi-console-render`). Chart x coordinates use sample timestamps and
break across long gaps; missing values are never synthesized as zero.
