<p align="center">
  <img src="res/icon_preview.png" width="128" alt="ImageHover icon">
</p>

<h1 align="center">ImageHover</h1>

<p align="center">See an image without opening it. Rest the mouse on an image file and a preview pops up.</p>

---

ImageHover is a small Windows tray app, written in plain C. When the mouse rests on an image file,
it shows a preview next to the cursor. It works in:

- **File Explorer** (all views: icons, list, details)
- **Open and Save dialogs** of other programs
- **The Desktop**

No install, no admin rights, no network access. One small `.exe`.

## Features

- **Fast previews.** Images load on a background thread. Recent previews are kept in memory, so
  going back to an image is instant.
- **Animated GIF and WebP** play in the preview.
- **PDFs** show page 1, drawn by the PDF reader built into Windows. No PDF program needed.
- **Videos, camera RAW, PSD and more** show the Windows thumbnail, when Windows has one.
- **Photo details** under the image: pixel size, file size, date taken (or modified), camera.
- **Photos are shown upright** (EXIF orientation is applied).
- **Settings:** hover delay, preview size, where previews show, "only while Shift is held",
  and turn animation, thumbnails and details on or off.
- **Start with Windows** (optional, current user only).
- **Pause** from the tray menu.

## Supported files

| Group | Extensions | Notes |
|---|---|---|
| Images | `.jpg` `.jpeg` `.jpe` `.jfif` `.png` `.gif` `.bmp` `.dib` `.tif` `.tiff` `.ico` `.jxr` `.wdp` `.dds` | Built into Windows |
| | `.webp` | Built into Windows 11 |
| | `.heic` `.heif` | Needs *HEIF Image Extensions* (Microsoft Store) |
| | `.avif` | Needs *AV1 Video Extension* (Microsoft Store) |
| | `.jxl` | Needs the *JPEG XL* extension (newer Windows 11) |
| PDF | `.pdf` | Page 1, built into Windows 10/11 |
| Video | `.mp4` `.m4v` `.mov` `.avi` `.wmv` `.mkv` `.webm` `.mpg` `.mpeg` `.3gp` `.ts` `.mts` `.m2ts` `.flv` | Still frame from the Windows thumbnail |
| Camera RAW | `.cr2` `.cr3` `.nef` `.nrw` `.arw` `.srw` `.dng` `.orf` `.rw2` `.raf` `.pef` `.3fr` `.erf` `.kdc` `.mrw` `.x3f` | Needs *Raw Image Extension* (Microsoft Store) on older Windows |
| Design | `.psd` `.svg` `.xcf` | Needs a program that adds thumbnails for these files |

Simple rule: if Windows Photos can open it, or Explorer shows a thumbnail for it, ImageHover can
preview it.

## Requirements

- Windows 10 or Windows 11, 64-bit

## Use

1. Run `ImageHover.exe`. An icon appears in the notification area (tray).
2. Rest the mouse on an image file. The preview appears after a short delay.
3. Move the mouse away or click to hide it.

Tray icon:

- **Left-click:** open Settings.
- **Right-click:** menu with *Settings*, *Pause/Resume*, *Start with Windows*, *Exit*.

Only one copy runs at a time.

## Build from source

You need [MinGW-w64](https://www.mingw-w64.org/) (gcc and windres) on the `PATH`.

```bat
build.bat          :: release build -> ImageHover.exe
build.bat debug    :: debug build, writes ImageHover.log next to the exe
```

Run the tests (no mouse needed, nothing is shown on screen):

```bat
tools\test_loader.bat
```

The icon is drawn by `tools/make_icon.py` (needs Python and Pillow). You only need it if you
change the icon:

```bat
python tools\make_icon.py
```

## Make a release

Needs git and the [GitHub CLI](https://cli.github.com) (`gh auth login` once).
Always start the script as `.\release.bat` from the project folder.

1. `.\release.bat -SetVersion 1.1.0` sets the new version and adds a `## [1.1.0]` section to `CHANGELOG.md`.
2. Write what was added, changed, fixed or removed in that section.
3. Commit and push.
4. `.\release.bat -DryRun` builds, tests, packs and shows the release notes. Nothing is published.
5. `.\release.bat` does the same, asks you to type `YES`, then creates the GitHub release
   (tag `v1.1.0`, zip and SHA256 attached, notes from `CHANGELOG.md`).
   Add `-Draft` to create a draft release first.

## Project layout

| Path | What it is |
|---|---|
| `src/main.c` | Tray icon, hover timer, start and exit |
| `src/locate.c` | Finds the file under the mouse (UI Automation + Shell) |
| `src/loader.c` | Background loader, image decode (WIC), thumbnails, cache |
| `src/pdf.c` | PDF page 1 through `Windows.Data.Pdf` |
| `src/render.c` | The preview window (rounded, layered, animated) |
| `src/settings.c` | Settings window, registry, Start with Windows |
| `res/` | Icon, manifest, settings dialog, version (`res/version.h`) |
| `tools/` | Test harness and icon script |
| `testdata/` | Small sample files for the tests |
| `CHANGELOG.md` | What changed in each version |
| `release.bat`, `release.ps1` | Build, test, pack and publish a GitHub release |

## How it works

Every 50 ms the app checks what is under the mouse with UI Automation. In File Explorer it asks
the Explorer window for the exact file (through `IShellWindows` and `IFolderView`). In dialogs
and on the Desktop it finds the folder and matches the file name. The image is decoded with the
Windows Imaging Component on a background thread and drawn in a click-through layered window.

## Privacy

ImageHover makes no network connections. It sends no data anywhere. It reads only the file under
the mouse.

## Settings storage and uninstall

Settings are stored in the registry, current user only:

- `HKEY_CURRENT_USER\Software\ImageHover`
- `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run` → value `ImageHover`
  (only when *Start with Windows* is on)

To uninstall:

1. Turn off *Start with Windows* in Settings, then *Exit* from the tray menu.
2. Delete `ImageHover.exe`.
3. Optional: delete the `HKEY_CURRENT_USER\Software\ImageHover` registry key.
