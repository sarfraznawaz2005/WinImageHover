# Changelog

All notable changes to ImageHover are listed here.
`release.bat` copies the section of the version being released into the GitHub release notes.

Sections per version: **Added**, **Changed**, **Fixed**, **Removed**. Empty sections are left out
of the release notes.

## [1.0.1] - 2026-11-02

First public release.

### Added
- Image preview when the mouse rests on an image file in File Explorer, Open/Save dialogs and on the Desktop.
- Exact file detection in File Explorer, so `photo.jpg` and `photo.png` are never mixed up.
- Background loading and a cache of recent previews, for fast and smooth previews.
- Animated GIF and WebP previews.
- PDF previews (page 1), using the PDF reader built into Windows. No PDF program needed.
- Previews for videos, camera RAW, PSD and other files through Windows thumbnails.
- Details under the image: pixel size, file size, date taken or modified, and camera.
- Photos are shown upright (EXIF orientation).
- Settings window: hover delay, preview size, where previews show, Shift-only mode,
  and on/off for animation, thumbnails and details.
- Start with Windows option.
- Pause and resume from the tray menu.
