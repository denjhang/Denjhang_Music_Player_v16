# YM2163 Piano GUI v9.0 - Package Information

## Package Details

**Package Name**: YM2163_Piano_v9.0_Release_2026-01-28.tar.gz
**Package Size**: 3.9 MB
**Build Date**: January 28, 2026
**Version**: v9.0 (Final)

---

## Package Contents

### Executable
- `ym2163_piano_gui_v9.exe` (4.4 MB) - Main application

### Source Code
- `ym2163_piano_gui_v9.cpp` (154 KB) - Complete source code
- `build_v9.sh` - Build script for compilation

### Configuration Files
- `ym2163_midi_config.ini` - MIDI instrument and drum mapping
- `ym2163_tuning.ini` - Frequency tuning settings
- `ym2163_folder_history.ini` - MIDI folder history (auto-generated)
- `imgui.ini` - ImGui window settings (auto-generated)

### Documentation
- `README.md` - Quick start guide
- `QUICK_START.md` - Detailed quick start
- `RELEASE_NOTES.md` - Original release notes
- `RELEASE_NOTES_v9.0_CN.md` - Complete Chinese release notes
- `RELEASE_NOTES_v9.0_EN.md` - Complete English release notes
- `VERSION_HISTORY.md` - Complete version history

### Libraries
- `ftd2xx64.dll` - FTDI driver library
- `ftdi_driver/` - FTDI driver files
- `imgui/` - ImGui framework source
- `midifile/` - MidiFile library source

---

## Installation

### Quick Install
1. Extract the tar.gz file:
   ```bash
   tar -xzf YM2163_Piano_v9.0_Release_2026-01-28.tar.gz
   ```

2. Navigate to the directory:
   ```bash
   cd YM2163_Piano_v9_Release
   ```

3. Run the application:
   ```bash
   ./ym2163_piano_gui_v9.exe
   ```

### Requirements
- Windows 10/11 (64-bit)
- SPFM hardware with YM2163 chip(s)
- FTDI USB driver installed
- 100MB free disk space

---

## Building from Source

### Prerequisites
- MinGW-w64 (GCC 15.2.0 or later)
- Make
- Git (optional)

### Build Steps
```bash
cd YM2163_Piano_v9_Release
./build_v9.sh
```

The build script will:
1. Compile ImGui sources
2. Compile MidiFile library
3. Compile main application
4. Link all components
5. Generate `ym2163_piano_gui_v9.exe`

---

## File Structure

```
YM2163_Piano_v9_Release/
├── ym2163_piano_gui_v9.exe      # Main executable
├── ym2163_piano_gui_v9.cpp      # Source code
├── build_v9.sh                  # Build script
├── ftd2xx64.dll                 # FTDI library
├── ym2163_midi_config.ini       # MIDI configuration
├── ym2163_tuning.ini            # Tuning settings
├── README.md                    # Quick start
├── QUICK_START.md               # Detailed guide
├── RELEASE_NOTES_v9.0_CN.md     # Chinese release notes
├── RELEASE_NOTES_v9.0_EN.md     # English release notes
├── VERSION_HISTORY.md           # Version history
├── PACKAGE_INFO.md              # This file
├── ftdi_driver/                 # FTDI driver files
│   ├── amd64/                   # 64-bit drivers
│   ├── i386/                    # 32-bit drivers
│   └── Static/                  # Static libraries
├── imgui/                       # ImGui framework
│   ├── imgui.cpp
│   ├── imgui_draw.cpp
│   ├── imgui_tables.cpp
│   ├── imgui_widgets.cpp
│   ├── imgui_impl_win32.cpp
│   └── imgui_impl_dx11.cpp
└── midifile/                    # MidiFile library
    ├── include/
    └── src/
```

---

## Configuration Files

### ym2163_midi_config.ini
Maps MIDI instruments (0-127) and drums to YM2163 settings:
- Instrument envelope (Decay/Fast/Medium/Slow)
- Instrument wave (String/Organ/Clarinet/Piano/Harpsichord)
- Drum bit mappings (BD/HC/SDN/HHO/HHD)

### ym2163_tuning.ini
Stores frequency tuning values for each note:
- 12 notes × 6 octaves = 72 tuning values
- Format: `C3=951` (FNUM value)
- Auto-saved when modified in tuning window

---

## First Run

On first run, the application will:
1. Create `imgui.ini` for window settings
2. Create `ym2163_folder_history.ini` for folder history
3. Load default MIDI configuration
4. Initialize FTDI connection

---

## Troubleshooting

### Application won't start
- Ensure `ftd2xx64.dll` is in the same directory
- Check Windows version (requires 64-bit)
- Install Visual C++ Redistributable if needed

### FTDI connection failed
- Install FTDI driver from `ftdi_driver/` folder
- Check USB connection
- Verify SPFM hardware is powered on

### MIDI files won't load
- Check file format (.mid or .midi)
- Verify file path length (< 260 chars recommended)
- Check file permissions

### Long path errors
- v9.0 supports paths > 260 characters
- If issues persist, move files to shorter path

---

## Uninstallation

Simply delete the `YM2163_Piano_v9_Release` folder. No registry entries or system files are modified.

---

## Support

For issues, questions, or feedback:
- Check documentation in the package
- Review release notes for known issues
- Visit project repository for updates

---

## License

This software is provided as-is. See project repository for license details.

---

## Credits

- **YM2163 Chip**: Yamaha Corporation
- **SPFM Hardware**: SPFM development team
- **ImGui**: Omar Cornut and contributors
- **MidiFile Library**: Craig Stuart Sapp
- **FTDI Driver**: Future Technology Devices International

---

## Version Information

- **Version**: v9.0
- **Build**: 2026-01-28-Final
- **Compiler**: MinGW-w64 GCC 15.2.0
- **Platform**: Windows 10/11 x64
- **Architecture**: x86_64

---

**Package Created**: January 29, 2026 00:02
**Package Hash**: (Calculate with `sha256sum YM2163_Piano_v9.0_Release_2026-01-28.tar.gz`)
