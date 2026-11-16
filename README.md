# HexEditor — A Cross-Platform Hex Editor Based on SOUI4

English | [中文](README_CN.md)

An HxD-style hex editor built with [SOUI4](https://github.com/soui4/soui) (a cross-platform C++ DirectUI framework; also mirrored on [Gitee](https://gitee.com/setoutsoft/soui4) for users in China).
The UI and rendering run entirely on SOUI's widget tree, while the logic layer (file model, search, undo stack) depends on no platform API at all,
so Windows / Linux / macOS share the exact same code base.

Build and run have been verified on **Windows 11 x64 (MSVC 2022)** and **Ubuntu 24.04**. Other platforms go through the `swinx` adaptation layer;
the source tree is organized for cross-platform builds, and macOS has not been tested yet (see "Known limitations" at the end).

---

## 1. Features

| Area | Capabilities |
| --- | --- |
| File | Open files of any size (**in milliseconds**), save, save as, pass a path directly on the command line |
| Display | Three-pane layout — offset column / hex column / ASCII column — zebra striping, 16-byte alignment |
| Selection | Mouse drag selection, double-click to select a run of printable characters, Ctrl+A select all, Shift+arrow keys to extend |
| Editing | Hex nibble input, direct ASCII typing, insert/overwrite modes, Backspace/Delete |
| Clipboard | Ctrl+C/V/X (provides both binary and Unicode text clipboard formats) |
| Undo | Ctrl+Z / Ctrl+Y, multi-level undo stack covering insert/delete/overwrite operations |
| Search | Boyer–Moore–Horspool, background thread with progress, text/hex modes, previous/next |
| Navigation | Ctrl+G go to offset (supports `0x` hex), F3 find next |
| Inspector | Right-hand pane shows live 8/16/32/64-bit unsigned and signed values of the same widths, Float32/64, and the character |
| Status bar | File size, cursor offset, selection length, current edit mode |
| Theming | Light / dark themes (switched from the View menu, remembered in `HexEditor.ini`), covering the hex area, caption bar, menu bar, toolbar, tab bar, status bar and data inspector, **including the drop-down popups of the main menu and the tab bar ▾** (all redrawn with SOUI's own `SMenuEx`) |

Keyboard shortcuts:

```
Ctrl+O Open          Ctrl+S Save        Ctrl+F Find         Ctrl+G Go to
Ctrl+Z Undo          Ctrl+Y Redo        F3      Find next
Ctrl+A Select all    Ctrl+C/V/X Copy/Paste/Cut
Tab    Switch hex/ASCII input column    Esc Clear selection
Home/End Start/end of line (with Ctrl: start/end of file)   PageUp/PageDown Page up/down
```

---

## 2. Why Large Files Open "Instantly"

```
Open file ──> CreateFileMapping + MapViewOfFile (Windows)
           └─> open + mmap (POSIX)
```

The file is mapped once and its contents are never read up front; pages are faulted in on demand by the OS, so opening a 40 GB file takes about as long as opening a 1 KB file,
and there is no "read a 40 GB file into memory" crash.

Writes never touch this read-only mapping (a side benefit: while you edit, other processes can still open the file read-only):

* **Page overlay (copy-on-write)**: 4 KB granularity; only modified pages are stored, so random overwrites cost a constant amount;
* **Piece table**: the logical stream is a sequence of pieces (live ranges of the base stream / inserted content). Inserting adds one piece,
  deleting drops the covered pieces, so inserting or deleting in the middle never moves the tens of gigabytes that follow;
  overwriting does not change the piece structure at all — it only writes to the page overlay or the insert buffer;
* **Insert buffer**: inserted content is appended sequentially to a `std::vector`; adjacent, contiguous pieces are merged automatically to suppress fragmentation;
* **Edit log**: the undo stack records OVR/INS/DEL operations and supports multi-level undo/redo (capped at 200 groups);
* **Save**: stream out in 1 MB blocks to a temporary file in the same directory → close the mapping → atomically replace the original → `mmap` again.
  (Windows refuses to overwrite a file that is still mapped, so the temp-file replace is mandatory — otherwise you get `ERROR_USER_MAPPED_FILE`.)

Search likewise allocates no large buffers: a 1 MB sliding window plus an overlap of `len(pattern)-1` bytes with the previous window
guarantees matches that straddle window boundaries are found; Boyer–Moore–Horspool bad-character skipping keeps the number of comparisons far below the file size.

---

## 3. Repository Layout

```
HexEditor/
├── CMakeLists.txt          Build entry point (add_subdirectory(soui4) + this project's targets)
├── README.md
├── README_CN.md
├── soui4/                  SOUI4 sources (github.com/soui4/soui, or gitee.com/setoutsoft/soui4 in China)
├── src/
│   ├── HexFileModel.h/.cpp mmap + page overlay + sparse edit pieces + undo stack + save
│   ├── HexSearch.h/.cpp    Boyer–Moore–Horspool background search
│   ├── HexView.h/.cpp      Custom SOUI control: three-pane painting, mouse/keyboard, scrolling, editing
│   ├── MainFrame.h/.cpp    Main window: toolbar, find bar, status bar, file dialogs
│   ├── InspectorWnd.h/.cpp Data inspector window
│   ├── Theme.h/.cpp        Light/dark theme definitions and application
│   ├── I18n.h/.cpp         Chinese/English UI language switching
│   ├── AboutWnd.h          About dialog          FindWnd.h   Find window
│   ├── PromptWnd.h         Prompt dialogs        CrtCompat.h CRT compatibility shims
│   ├── main.cpp            SOUI application initialization and entry point
│   └── resource.h
└── uires/
    ├── uires.idx           Resource index
    ├── uidef/init.xml      Font/color/style definitions
    ├── layout/main.xml     Main window layout
    └── values/*.xml        Strings/colors/skins
```

---

## 4. Building

### Windows

Requirements: CMake ≥ 3.16, Visual Studio 2022 (or any MSVC v142+ toolset), Windows 10 SDK.

```bash
# 1) Get SOUI4 (if the soui4/ directory does not exist yet)
git clone --depth 1 https://github.com/soui4/soui.git soui4
# In China, use the Gitee mirror instead (the two are the same code base):
# git clone --depth 1 https://gitee.com/setoutsoft/soui4.git soui4

# 2) Configure (run inside an "x64 Native Tools Command Prompt for VS 2022")
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# 3) Build
cmake --build build --config Release
```

A single-configuration generator such as NMake works too (equivalent, and handy for command-line/CI builds):

```bash
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Artifacts land in `build/bin/`: `HexEditor.exe`, `soui4.dll`, `utilities4.dll`, `render-gdi.dll`,
`imgdecoder-stb.dll`, `soui-sys-resource.dll`, plus the `uires/` resource directory.

Run: `build\bin\HexEditor.exe [file path]`

> Note: `CMakeLists.txt` only checks that the SOUI4 sources exist under `soui4/` (or a directory passed via `-DSOUI4_ROOT=...`);
> if they are missing, its error message prints the Gitee clone command. Outside China, clone from the GitHub address above instead — the effect is identical.

### Switching to the Skia Renderer

GDI rendering is the default (fewest dependencies). For Skia (better text anti-aliasing), change one line in `src/main.cpp`:

```cpp
cfg.SetRender(Render_Skia)     // was Render_Gdi
```

and make sure `render-skia.dll` and the Skia libraries are built (CMake already lists them as dependencies).

### Linux Build (Ubuntu 24.04, Tested)

```bash
sudo apt install build-essential ninja-build pkg-config libssl-dev \
  libxcb1-dev libxcb-render0-dev libgl1-mesa-dev libglu1-mesa-dev \
  freeglut3-dev uuid-dev libasound2-dev libfontconfig1-dev libfreetype-dev \
  libxkbcommon-dev libdbus-1-dev

cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
# Note: these are dlopen-loaded plugins, not link dependencies, so they must be built explicitly
ninja -C build-linux HexEditor resprovider-zip msftedit translator log4z
```

Running requires an X session; with no `DISPLAY` the process segfaults directly (139). In CI, use
`Xvfb :99 -screen 0 1280x800x24` together with `DISPLAY=:99`.

Two system fonts are required, otherwise parts of the UI render as boxes: `fonts-noto-cjk` (Chinese), and the icon font
**Segoe MDL2 Assets** (`C:\Windows\Fonts\segmdl2.ttf` — the toolbar/caption-bar/menu check marks use its code points).
swinx's Linux back end implements `AddFontResourceW` (via fontconfig's `FcConfigAppFontAddFile`),
so a release build can ship the icon font inside `uires/font/` and register it at startup (note: that font is Microsoft-licensed and may not be redistributed;
for a formal release, switch to an open icon font or SVG).

#### File Dialogs (Open / Save As)

Windows uses COM's `IFileDialog`; **Linux uses swinx's `GetOpenFileNameW` / `GetSaveFileNameW`**,
which internally tries, in order: xdg-desktop-portal (D-Bus) → the KDE/GNOME portals → the `zenity` command-line tool.
So on Linux a working file picker must be installed, otherwise clicking the menu simply does nothing:

```bash
sudo apt install zenity
```

(In a pure Xvfb environment there is no D-Bus session, so it falls straight through to the zenity branch — verified working.)
Also note that swinx passes `OPENFILENAME.lpstrInitialDir` verbatim as zenity's `--filename`,
so this code passes **the full path of the current file**, which lands the dialog in that file's directory with the file pre-selected.

#### Cross-Platform Pitfalls (All of These Only Surfaced on Linux)

| Symptom | Root cause | Fix |
|---|---|---|
| Toolbar/caption icons are all empty boxes | The Segoe MDL2 Assets font is missing; every glyph is `.notdef` | Install the font (see above) |
| Inspector shows only the first letter `s` of the file name | **In glibc's `swprintf`, `%s` means `char*`; only MSVC treats it as `wchar_t*`** — the wide string is read as narrow characters and truncated at the first `0x00` | Replace bare `swprintf` with `SStringT::Format` |
| Menu items look like `Title<box>Ctrl+F`, shortcut not right-aligned | Under the Skia renderer, `\t` (U+0009) has no glyph | In `SMenuExItem::OnPaint`, split on `\t` into left/right parts and draw the right part with `DT_RIGHT` |
| Menu check mark invisible in the dark theme | A `<circle>` inside `menu_check_dark.svg` makes the whole image fail to decode | Replace circles/dots with equivalent `<path>` arcs |
| Inspector's `txt_file` shows only 1 character | A `multiLines` text in a vbox gets a wrap-content width of 1 character | Give it a fixed `size="280,20"` and drop `multiLines` |
| Inspector heading "Data Inspector" invisible in dark theme | `ApplyTheme` only `Invalidate()`d the content subtree; the caption area was never repainted | Call `Invalidate()` on the whole window at the end of `ApplyTheme` |
| Inspector's `Float32`/`Double64` values are clipped at the end | DejaVu Sans Mono is about 10% wider than Consolas | Default width 270 → 300 |
| "File → Open/Save As" does nothing when clicked | `PickFile()` was a stub returning `FALSE` on the non-Windows branch | Use `GetOpenFileNameW`/`GetSaveFileNameW` (see above; requires zenity) |
| Combo box text `16` sits too high | `m_nFontHeight` comes from `MeasureText("A")`; Skia returns the glyph bounding box (`-fTop+fBottom`), not the line height. When the font box is taller than the edit box, `(client height − font height)/2` goes negative and the richedit view is pushed out of the client area | **Do not "fix" it**: using `tmHeight` instead shifts text 3.5px too low (richedit draws flush against the top of the view, so `1px of padding = 1px of shift`). The current value is optically optimal; a real fix would have to change richedit41's line positioning |

Two more patches were made at the framework layer (swinx, platform-specific files only, no effect on Windows):
`SetMenuItemInfo` now handles `MIIM_FTYPE`/`MIIM_STATE` (otherwise `MF_OWNERDRAW` is lost and menus fall back to default painting);
and both the default menu painting branch and cairo's `TextLayoutEx` handle `\t`.

---

## 5. Design Notes

* **Control**: `CHexView` derives from `SWindow` and registers via `DEF_SOBJECT(SWindow, L"hexview")`;
  `main.cpp` calls `app.RegisterWindowClass<CHexView>()`, after which XML can simply write `<hexview .../>`.
  Painting uses only `IRenderTarget` (draw text/rectangles, select fonts), and all coordinates are client-area-local,
  with SOUI handling double buffering and window composition.
* **Edit modes**: overwrite mode rewrites the current byte; insert mode inserts new bytes at the cursor (the file grows).
* **Data flow**: `HexFileModel` knows nothing about the UI, and `CHexView` knows nothing about the file system;
  they interact through `IHexSource` (used by search) and direct interface calls, which keeps unit testing easy.
* **Cross-platform**: file operations, threads and the clipboard each have two implementations split by `#ifdef _WIN32 / #else`;
  the UI layer stays a single code base through SOUI + `swinx`.
* **Theming**: `src/Theme.h/.cpp` defines `STheme` (a `SHexPalette` for the hex area plus `SChrome` for the window frame),
  one set for light and one for dark. `CMainFrame::ApplyTheme()` dispatches it:
  - The hex area uses `CHexView`'s own `colorBg/colorBgAlt/colorOffset/colorHex/colorAscii/colorSelBg/colorSelText/colorCaret/colorGrid` attributes; these `ATTR_COLOR`s are all
    `bInvalidate=FALSE`, so after changing them you must call `Invalidate()` manually.
  - The frame uses `SetAttribute(L"colorBkgnd"/L"colorText"/...)` to change the background and text colors of plain windows
    (the background is filled by `SWindow::OnEraseBkgnd`); menu item text color comes from `<btnstyle>` and is not inherited from the parent,
    so the menubar's child windows have to be walked and updated one by one.
  - The data inspector is a separate `SHostWnd`, themed separately by `CInspectorWnd::ApplyTheme()`.
    Its value rows are `SEdit` (`SRichEdit` does not paint its own background — it BitBlts the parent window's background),
    so it is enough to set `colorBkgnd` on the content container and `colorText` on the value rows.
  - Toolbar icons and window buttons use Segoe MDL2 Assets glyphs, so changing `colorText` suffices — no skin swap needed.
  - **The toolbar's two drop-downs** (bytes per row / character set) need four separate treatments: the combo box's own `colorBkgnd`,
    the inner `edit` child window's `colorText` (children do not inherit from the parent), the drop-down arrow's `btnSkin`
    (dark theme uses the `skin_dropbtn_dark` skin registered from `svg/dropbtn_dark.svg`),
    and the drop-down list from `SComboBox::GetListBox()` (`colorBkgnd`/`colorText`).
  - **Tab bar**: in the dark theme, `tabctrl_ex`'s `tabSkin` switches to the `colorrect` skin `skin_tab_dark`
    (the selected state goes through `PushDown` — `STabCtrl::OnPaint` paints the current page with `WndState_PushDown`);
    the three scroll/list buttons on the right originally had no `skin` and used the system default button skin (light background),
    so they were set to `skin="skin_tb_btn"` (transparent, flat) before `colorBkgnd`/`colorText` would take effect.
  - **Popup menus**: both the main menu and the tab bar ▾ list were converted to SOUI's self-drawn `SMenuEx` so they follow the theme.
    - `main.xml`'s `menubar` enables `useMenuEx="1"`; everything under `uires/smenu/*.xml` was changed to
      **a `menuRoot` root node with `menuItem` children** (the native `SMenu` parser expects `<menu>`/`<item>`,
      while `SMenuEx` only recognizes `menuRoot`/`menuItem` — with the wrong tags the menu is simply empty).
    - The tab list's template is `uires/smenu/menu_tablist.xml` (an empty `menuRoot`);
      its entries are added at runtime with `AppendMenu`.
    - Dark skins: `skin_menu_dark` (item background, selection `#094771`) / `skin_menu_sep_dark` / `skin_menu_check_dark` / `skin_menu_arrow_dark`.
  - **Two hard constraints of `SMenuEx`** (both learned the hard way):
    1. Tag names must be `menuRoot` / `menuItem`, otherwise the menu is empty and opens to nothing;
    2. **The skin must be settled before the items are created** — the `SMenuExItem` constructor copies the root's
       `m_pItemSkin` (`SMenuEx.cpp:537`), so changing root attributes afterwards has no effect on items that already exist
       (symptom: the text color changed but the background stayed light).
       Theme switches therefore **rebuild the menu**: `DestroyMenu()` → inject the theme attributes into the XML root node
       → `LoadMenu2()`. That logic lives in `CMainFrame::ApplyChromeTheme()`, and the order of `kMenuRes[]` must match the declaration order of `<menus>` in `main.xml`.
> **Menu skins are rebuilt only once, at startup; switching the theme at runtime no longer touches the menus.**
> Reason: inside `SMenuBar::PopMenu()`, `m_pHostMenu->m_pNowMenu` is the "a menu is currently popped" state flag.
> If an `SMenuEx` window is destroyed/rebuilt during a menu session (`DestroyMenu()+LoadMenu2()`),
> `PopMenu`'s state is corrupted — from then on, every click hits `if (m_pNowMenu != NULL) return 0;` at the entry point and returns immediately,
> **and the menus never pop up again until the app is restarted**.
> So `RebuildMenuSkins()` is called exactly once, in `OnInitDialog` (at that point the menu window has never been shown, which is safe).
> Cost: after switching the theme at runtime, the drop-down menus' colors only follow **on the next launch**;
> the caption bar / menu bar / toolbar / tab bar / status bar / editing area / inspector all switch instantly.
  - The native menus' dark-mode switches are wired up too, but **proved ineffective in testing** (kept anyway — they may work on another machine):
    uxtheme's `SetPreferredAppMode`(135) / `FlushMenuThemes`(136) / `AllowDarkModeForWindow`(133), see `Theme::ApplyNativeMenuTheme()` and
    `Theme::AllowDarkWindow()`; plus the `supportedOS` declaration in `src/app.manifest`,
    embedded via `1 24 "app.manifest"` in `HexEditor.rc` (the type must be written as the number 24).
* **Remember to re-configure after adding a .cpp**: `CMakeLists.txt` enables `file(GLOB ... CONFIGURE_DEPENDS)`,
  so new files trigger an automatic re-configure; without it they never enter the build (symptom: `LNK2019 unresolved external symbol`).
* **After changing a header, this project's sources must be rebuilt in full** (`CMakeLists.txt` already forces this with `OBJECT_DEPENDS`).
  The pitfall that taught us this: after adding members to `CHexView`, only `HexView.cpp` was recompiled, while `main.cpp`/`MainFrame.cpp`
  still used the old `sizeof(CHexView)` to `new` the object, and the constructor wrote members at the new layout → **heap buffer overflow**,
  showing up as a random `0xC0000374` (comes and goes; adding two lines of logging can make it "disappear", so it is easily mistaken for a bug in the overlay painting code).

### Overlay Scrollbars (Windows 11 Style)

`CHexView`'s scrollbars **do not use SOUI's layout-embedded scrollbars**; they are self-drawn as an overlay on top of the content:

* `main.xml` declares `<hexview sbWid="0" sbArrowSize="0"/>` — SPanel no longer reserves layout space for scrollbars
  and does not paint them either (`sbWid` must be 0; any non-zero value leaves a permanent empty slot).
* At rest: a 6px thin bar with a rounded capsule thumb (`FillSolidRoundRect`; its color is derived automatically from the brightness of `m_crBg`); 
  on hover: it widens to 14px and paints the track plus arrow triangles at both ends. **The layout never changes and the content never reflows.**
* **The thumb's travel must exclude the arrows at both ends**: each end of the track has a 14px arrow-button zone, and the thumb may only move within
  `SbThumbTrack()` (= the track inset by one arrow width), otherwise dragging to the top would cover the up arrow.
  **The rest state must apply the same inset even though it paints no arrows** — otherwise, when the arrows appear on hover, the thumb gets pushed down by 14px
  and appears to "jump". `SbThumb()` and `SbUpdateDrag()` must use the same interval,
  or the painted position and the drag conversion will disagree.
* Both directions are supported (`SbNeedV()`/`SbNeedH()` go through `HasScrollBar()`; the scroll position comes from
  `GetScrollBarInfo()`'s `nMax/nPage/nPos`, so it naturally agrees with wheel/keyboard scrolling).
* Interaction: `SbHandleDown()` starts a drag if it hits the thumb, otherwise pages by a full page; `SbUpdateDrag()` converts the position and calls
  `SetScrollPos()`; `OnMouseMove` updates the hover state first, then runs the original content hit-testing.
* **Cursor**: `OnSetCursor()` always returns the arrow over the scrollbar area — and **for the whole duration of a scrollbar drag**.
  While dragging, the pointer easily slides onto the hex/ASCII content; without the special case it would switch to an I-beam, which feels wrong.
  `OnMouseMove` also has a drag safeguard: if `nFlags` has no `MK_LBUTTON` (the button was released outside the window, so no
  `WM_LBUTTONUP` arrives), the drag ends immediately — otherwise you are left with "the cursor is stuck as an arrow and any pointer movement keeps scrolling".
* Two pitfalls:
  1. **Mouse messages carry host client-area coordinates**; you must convert with `ToLocalPoint()` before hit-testing;
  2. `FillSolidRect`/`FillSolidRoundRect` go through `SColor(cr)`, whose **high byte is alpha** —
     passing a GDI `RGB()` value (high byte 0) paints fully transparent. This project's theme colors come from XML (ARGB), so they are safe.
* **Arrow direction**: in `SbDrawArrow(pRT, rcCell, bVert, bToStart, cr)`, `bToStart` means
  "the tip points toward the start side" — for the vertical direction the start is the **top**, for horizontal it is the **left**. So
  the top/left arrows pass `true` (up-pointing / left-pointing triangle) and the bottom/right arrows pass `false` (down-pointing / right-pointing triangle).
  This parameter set was initially passed backwards (both vertical and horizontal), and has been fixed.
* Two debug switches (only for automated verification; not needed in normal use):
  `HEX_FORCE_HOVER=1` locks the hover state (on this machine the foreground is often taken by other processes, the real cursor never enters the window,
  and the system immediately issues a compensating `WM_MOUSELEAVE`); `HEX_OV_OFF=1` disables the overlay entirely.
  `tools/probe_overlay.py` verifies by screenshot using `PrintWindow(PW_RENDERFULLCONTENT)` plus directly posted mouse messages.

---

## 6. Known Limitations / Future Work

* Non-Windows platforms: Ubuntu 24.04 is verified to build and run — menus / toolbar / find & replace / About / data inspector /
  dark & light themes / Chinese–English switching / context menus have all been checked. File dialogs on Linux go through swinx's
  `GetOpenFileNameW` / `GetSaveFileNameW` (xdg-desktop-portal or zenity, see Section 4); the clipboard on non-Windows platforms still falls back to an in-process buffer.
  macOS has not been tested yet.
* A 32-bit process cannot map a >2 GB file as a whole (address-space limit of Windows' `MapViewOfFile`);
  this needs a segmented mapping window. 64-bit targets are unaffected.
* No character-set switching yet (the ASCII view uses a fixed Latin-1 printability test), no diff highlighting, no bookmarks.
* The ASCII column does not handle double-byte characters such as GBK yet (Latin-1 only).
* Drag selection does not use `SetCapture`: SOUI's control-level mouse capture is unreliable in its release timing, and a missed release swallows later clicks
  (toolbar buttons stop responding). Drag selection currently relies on mouse-move messages plus edge auto-scrolling, and pauses when the pointer leaves the control.
* The right-aligned "Undo/Redo/Overwrite" group in the toolbar can be pushed out of view in a narrow window — a layout item still to be optimized.

## 7. Tests

* `hexeditor-test` (no UI dependency): 400 rounds of randomized "overwrite / insert / delete / truncate" compared byte-for-byte against a `std::vector`
  reference model; step-by-step undo/redo verification over 120 operations; save/reopen consistency;
  instant mmap opening of a 512 MB file and an insertion in its middle. All passing.
* `tools/gui_smoke.py`: a real-window end-to-end run — click a hex cell → type hex characters → click Save,
  then verify that exactly one byte of the file changed.
