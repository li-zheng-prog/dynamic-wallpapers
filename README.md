# Dynamic Wallpapers

A Windows desktop app that automatically switches between light and dark wallpapers
on a schedule or with the Windows app theme. Built with C++ and Direct2D, with a
custom interface inspired by WinUI 3.

**Requires Windows 10 or later.**

## Contents

- [Features](#features)
- [Download and install](#download-and-install)
- [Getting started](#getting-started)
- [Themes and configuration](#themes-and-configuration)
- [Command-line options](#command-line-options)
- [Development](#development)
- [License and acknowledgments](#license-and-acknowledgments)

## Features

| Feature | Description |
| --- | --- |
| Automatic switching | Set day and night start times, or follow the Windows app theme, including Custom color mode. |
| Theme management | Browse a responsive card grid, create themes from two images, rename themes, and move unwanted themes to the Recycle Bin. |
| Wallpaper positioning | Choose center, tile, stretch, fit, fill, or span across monitors. |
| App appearance | Choose system, light, or dark appearance, set the app language, and select Chinese and English fonts independently. |
| Window effects | Use live system acrylic on supported Windows 11 versions, or a solid background on Windows 10. Custom wallpaper-based blur is also available. |
| Background operation | Keep switching from the system tray and optionally start silently with Windows. |
| Native integration | Native minimize and restore animations, support for system animation settings, and IME input for theme names. |

## Download and install

Download the installer, portable executable, or example theme archive from
[Releases](../../releases):

| File | Use |
| --- | --- |
| [DynamicWallpapersSetup.exe](https://github.com/li-zheng-prog/dynamic-wallpapers/releases/latest/download/DynamicWallpapersSetup.exe) | Install the app with an example theme and a Windows uninstall entry. |
| [DynamicWallpapers-portable.exe](https://github.com/li-zheng-prog/dynamic-wallpapers/releases/latest/download/DynamicWallpapers-portable.exe) | Run the app without installation. |
| [themes.zip](https://github.com/li-zheng-prog/dynamic-wallpapers/releases/latest/download/themes.zip) | Add the example theme to the portable app. |

### Installer

Run `DynamicWallpapersSetup.exe` and choose **简体中文** (Simplified Chinese) or
**English**. Setup initially selects a language based on the Windows display
language and uses the selected language for setup and the uninstaller. The app's
language setting is independent and follows the Windows display language by default.
The default installation directory is `%LOCALAPPDATA%\Programs\Dynamic Wallpapers`;
administrator rights are not required. You can uninstall the app from Windows Settings.

### Portable app

Place `DynamicWallpapers-portable.exe` in a folder of your choice. Extract
`themes.zip` so that the `themes` folder sits beside the executable:

```text
Dynamic Wallpapers/
├── DynamicWallpapers-portable.exe
└── themes/
    └── Big Sur Graphic/
        ├── theme.json
        ├── Big Sur Graphic_1.jpg
        └── Big Sur Graphic_2.jpg
```

You can also launch the app without the example theme and create your own with
**Add theme**. The portable app stores settings in the user data directory;
theme locations are described below.

## Getting started

1. Launch the app and select a theme, or choose **Add theme** to create one from
   a light image and a dark image.
2. Choose a wallpaper switching mode:
   - **Schedule**: set **Day starts** and **Night starts**. The defaults are 06:00
     and 18:00.
   - **System**: follow the Windows app theme. The schedule is inactive in this mode.
3. Enable **Switch wallpapers automatically** and choose a wallpaper position.
4. Optionally enable startup with Windows to launch silently in the system tray.

The app's **System / Light / Dark** appearance setting controls its interface
separately from the wallpaper switching mode.

Under **Appearance**, use the **System / Chinese / English** buttons to select
the app language. Choose Windows typography or an installed font separately for
Chinese and English, including in mixed text. Changes apply immediately and are
saved for the next launch. The main window can be resized below its starting size;
scroll to reach settings that do not fit.

### System tray

By default, closing the window leaves the app running in the system tray. Click
the tray icon to reopen it, or right-click to change settings, toggle automatic
switching, choose **Apply now**, or **Quit**.

### Manual wallpaper changes and retries

Switching manually between the selected theme's own images keeps that theme
active. When you choose an unrelated wallpaper, the app waits for it to remain
stable for about one second and confirms the change before clearing the theme
selection. Automatic wallpaper writes pause during this check; delayed
notifications and staggered monitor updates do not immediately clear the theme.

The cleared selection persists across restarts. Select a theme again to resume
using it. Failed or timed-out wallpaper changes are attempted at most three
times; choose **Apply now** or select the theme again to retry.

## Themes and configuration

### Create and manage themes

Use **Add theme** to select a light image, a dark image, and a name. The app creates
the theme files for you. Theme names support IME input, and deleting a theme moves
its folder to the Recycle Bin.

Supported image extensions are `.jpg`, `.jpeg`, `.png`, and `.bmp`.

### Theme format

Each theme occupies its own folder containing images and an optional
WinDynamicDesktop-compatible `theme.json`. The app uses one day image and one
night image. For example, the bundled theme contains:

```json
{
  "imageFilename": "Big Sur Graphic_*.jpg",
  "displayName": "Big Sur Graphic",
  "dayImageList": [1],
  "nightImageList": [2]
}
```

The `*` is replaced by the image number, so this example selects
`Big Sur Graphic_1.jpg` for daytime and `Big Sur Graphic_2.jpg` for nighttime.
When a list contains multiple entries, only its first entry is used.

Without `theme.json`, the first sorted image is used for daytime and the last for
nighttime. Files are sorted by the number after the final underscore, then by
filename; names such as `Wallpaper_1.jpg` and `Wallpaper_2.jpg` make the order
explicit.

### File locations

| Data | Location |
| --- | --- |
| Settings | `%APPDATA%\Dynamic Wallpapers\config.ini` |
| Bundled or manually added themes | `themes\` beside the executable |
| Themes created or imported in the app | `themes\` beside the executable when writable; otherwise `%APPDATA%\Dynamic Wallpapers\themes\`. |

The app also migrates settings from the legacy `%APPDATA%\TimeWall` directory and
scans its themes directory as a fallback.

## Command-line options

Run these options with the app executable. For example, in PowerShell:

```powershell
.\DynamicWallpapers-portable.exe --show
.\DynamicWallpapers-portable.exe "--set-theme=Big Sur Graphic"
.\DynamicWallpapers-portable.exe --apply
```

### App and theme actions

| Option | Action |
| --- | --- |
| `--show` | Show the main window. |
| `--apply` | Apply the current wallpaper once and exit. |
| `--set-theme=<name>` | Select a theme; notify the running instance if one exists. |
| `--add-theme=<folder>` | Import a theme's day and night images from a folder and open the main window. |
| `--delete-theme=<name>` | Move a theme to the Recycle Bin and exit. |
| `--autostart` | Start silently in the system tray. |

The app normally runs as a single instance. Close it before using `--add-theme`
or `--delete-theme`; these actions return exit code `4` if an instance is already
running.

### Appearance and diagnostics

| Option | Effect |
| --- | --- |
| `--self-glass` | Use custom wallpaper-based blur with tint and subtle noise. |
| `--solid` | Use a solid background without blur. |
| `--opaque` | Use an opaque background for diagnostics. |
| `--simulate-hour=<n>` | Override the current hour for testing. |

Live system acrylic is enabled by default where supported on Windows 11 22H2 or
later. Windows 10 uses a solid background that follows the app appearance.

## Development

### Requirements

- Windows with [MinGW-w64](https://winlibs.com/) (`g++` and `windres`).
- Git Bash to run the build script.
- Python 3 with Pillow for icon generation.

The build script defaults to the compiler tools in `D:\mingw64\bin` and uses
`D:\DynamicWallpapers_build` as a temporary build directory to avoid issues with
non-ASCII source paths. Update `GXX`, `WINDRES`, and `BUILD` in
[`build/build.sh`](build/build.sh) if your paths differ. The script recreates the
build directory on each run.

### Build

From the repository root in Git Bash:

```bash
python -m pip install Pillow
bash build/build.sh
```

The build produces:

```text
dist/
├── DynamicWallpapers-portable.exe
├── DynamicWallpapersSetup.exe
└── themes/
```

The app, installer, and Windows shortcuts share artwork generated from
`logo.png`, an opaque 1024 x 1024 master centered on the original black-and-white
mark. The Windows exports add a continuous rounded-square mask, including the
16 px tray icon and both title-bar images. When replacing the artwork, bump
the installed icon filename in `src/shell_identity.h` and `build/build.sh` so
Windows loads the new version instead of reusing a cached icon.

The installer embeds the application and repository themes. The installer,
portable executable, and `themes.zip` are tracked in Git under `dist/`.
Additional generated files, including the `dist/themes/` directory, are ignored.

### Run regression tests

Run the suite in a Windows desktop session with real Direct3D and Windows Imaging
Component (WIC) support:

```powershell
powershell -ExecutionPolicy Bypass -File build/test.ps1
```

If the compiler is installed elsewhere, pass its path:

```powershell
powershell -ExecutionPolicy Bypass -File build/test.ps1 -Compiler "C:\mingw64\bin\g++.exe"
```

The suite builds a separate test executable in a temporary directory. It covers:

- Wallpaper deduplication, delayed and staggered monitor updates, rapid reversals,
  external-change confirmation, and bounded retries.
- Deferred graphics initialization, DPI buffer reuse, native minimize and restore,
  and window placement.
- Asynchronous thumbnail cancellation and shutdown.

Wallpaper writes, configuration saves, and startup registry writes are stubbed
during testing.

### Repository layout

| Directory | Purpose |
| --- | --- |
| [`src/`](src/) | Windows application and custom Direct2D interface. |
| [`setup/`](setup/) | Installer with its payload embedded as RCDATA resources. |
| [`build/`](build/) | Build, icon generation, payload packaging, and test scripts. |
| [`dist/`](dist/) | Installer, portable executable, and example theme archive. |
| [`tests/`](tests/) | Startup, rendering, and wallpaper regression tests. |
| [`themes/`](themes/) | Example themes distributed with the installer. |

## License and acknowledgments

The code is released under the [MIT License](LICENSE).

The wallpaper images in `themes/Big Sur Graphic/` are copyrighted by Apple and
are included as examples for personal use and demonstration. Consider the
applicable image rights before redistributing an installer containing them.

Thanks to [WinDynamicDesktop](https://github.com/t1m0thyj/WinDynamicDesktop) for the
theme format and the approach to wallpaper switching.
