# Dynamic Wallpapers

A Windows desktop app that switches between a theme's light and dark wallpapers on a
schedule or with the Windows app theme. Built with a custom WinUI 3 style interface.

## Features

- **Theme management**: a scrolling, responsive card grid. Create a theme from two
  images, rename it, or move it to the Recycle Bin. Supports the WinDynamicDesktop
  `theme.json` format.
- **Switching modes**: set day and night start times, or follow the Windows app theme.
  Changes take effect immediately, including when Windows uses Custom color mode.
- **Appearance**: system, light, or dark, with the Windows accent color.
- **Wallpaper positioning**: center, tile, stretch, fit, fill, or span across monitors.
- **Window background**: live system acrylic on Windows 11 and a theme-aware solid
  background on Windows 10. `--self-glass` enables a custom wallpaper-based blur
  with a tint and subtle noise.
- **System tray**: closing the window keeps wallpaper switching active. The tray
  menu provides settings, pause, Apply now, and Quit actions.
- **Native minimize and restore**: uses Windows animations and respects system
  animation settings.
- **Start with Windows**: launches silently in the system tray.
- **Single instance**: forwards supported commands to the running instance.
- **Manual wallpaper changes**: switching between the selected theme's own images
  keeps that theme active. An external wallpaper must remain stable for about one
  second and pass a follow-up check before the theme is deselected. Automatic
  writes pause during that check. No-theme state persists across restarts until
  another theme is selected. Delayed notifications and staggered monitor updates
  do not immediately clear the theme. Failed or timed-out changes are attempted
  at most three times; use Apply now or select the theme again to retry.
- **Unicode input**: theme name fields support input method editors (IMEs).

See `src/` and `setup/` for implementation and uninstall details.

## Download and install

Download the files from [Releases](../../releases):

| File | Platform | Description |
|---|---|---|
| `DynamicWallpapersSetup.exe` | Windows 10+ | Installer with an example theme and an uninstall entry |
| `DynamicWallpapers-portable.exe` | Windows 10+ | Portable app; place a `themes/` folder beside it |
| `themes.zip` | Windows 10+ | Example theme for the portable app; extract beside the executable |

## Build from source

### Windows (MinGW-w64)

Requirements: [MinGW-w64](https://winlibs.com/) (`g++`, installed at `D:\mingw64`,
or update the paths in `build/build.sh`), Git Bash, and Python 3 with Pillow.

```bash
bash build/build.sh
# Outputs: dist/DynamicWallpapers-portable.exe and dist/DynamicWallpapersSetup.exe
```

Run the regression suite in a Windows desktop session with real D3D/WIC support:

```powershell
powershell -ExecutionPolicy Bypass -File build/test.ps1
```

The suite builds a separate test executable in a temporary directory. It covers
wallpaper deduplication, delayed and staggered monitor updates, rapid reversals,
external-change confirmation, bounded retries, deferred graphics initialization,
DPI buffer reuse, native minimize/restore and window placement, asynchronous
thumbnail cancellation, and shutdown. Wallpaper writes, configuration saves, and
startup registry writes are stubbed during testing.

## Theme format

Each theme is a folder under `themes/` containing images and an optional
WinDynamicDesktop-compatible `theme.json`. This app uses one day image and one
night image:

```json
{
  "imageFilename": "Big Sur Graphic_*.jpg",
  "displayName": "Big Sur Graphic",
  "dayImageList": [1],
  "nightImageList": [2]
}
```

Without `theme.json`, the first naturally sorted image is used for light mode and
the last for dark mode. Use **Add theme** to create a theme from two selected images.

## Repository layout

```text
src/        Windows application with a custom Direct2D interface
setup/      Installer with its payload embedded as RCDATA resources
build/      Build, icon generation, payload packaging, and test scripts
tests/      Startup, rendering, and wallpaper regression tests
themes/     Example themes distributed with the installer
```

## Command-line options

```text
--show                 Show the main window
--apply                Apply the current wallpaper once and exit
--set-theme=<name>     Select a theme; forward to the running instance if present
--add-theme=<folder>   Import images from a folder and open the main window
--delete-theme=<name>  Move a theme to the Recycle Bin and exit
--autostart            Start silently in the system tray
--simulate-hour=<n>    Override the current hour for testing
--opaque               Use an opaque background for diagnostics
--self-glass           Use custom frosted glass instead of system acrylic
--solid                Use a solid background without blur
```

## License

The code is released under the [MIT License](LICENSE).

The wallpaper images in `themes/Big Sur Graphic/` are copyrighted by Apple and
are included as examples for personal use and demonstration. Consider the
applicable image rights before redistributing an installer containing them.

## Acknowledgments

- [WinDynamicDesktop](https://github.com/t1m0thyj/WinDynamicDesktop) for the theme
  format and the approach to wallpaper switching.
