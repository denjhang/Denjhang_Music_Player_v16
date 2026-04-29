# YM2163 Piano GUI v9.0 Release Notes

## Release Date
January 28, 2026

## Version Information
v9.0 (Build 2026-01-28-Final)

---

## 🎉 Major New Features

### 1. Dynamic Drum Allocation System
- **Smart Chip Allocation**: In dual-chip mode, drums alternate between two chips
- **Independent UI Display**: Each chip displays drum status separately, no longer lighting up simultaneously
- **Real-time Logging**: Shows current chip number for debugging
- **Applicable to**:
  - Numpad 1-5 triggers
  - UI drum button clicks
  - Drums in MIDI playback

### 2. File Browser Highlight System
- **Exited Folder Highlight**:
  - When returning to parent directory, the just-exited folder shows yellow highlight
  - Highlight persists until entering another folder
  - Auto-scrolls to highlighted folder position
- **Playing Path Highlight**:
  - Folder containing currently playing file shows light blue highlight
  - Parent directories in path also show light blue highlight
  - Easy to identify current playing file location

### 3. Filename Scrolling Display
- **Auto-detection**: Detects long filenames (exceeding display width)
- **Trigger Conditions**:
  - Hovered items (no click needed)
  - Selected items
  - Highlighted folders
- **Scroll Animation**:
  - Slowly scrolls left to right (30 pixels/second)
  - Pauses 1 second at right end
  - Slowly scrolls right to left
  - Pauses 1 second at left end
  - Loops continuously
- **Applicable to**: Both MIDI files and folders

### 4. Scroll Position Memory
- **Auto-save**: Saves current scroll position every frame
- **Auto-restore**: After entering subdirectory and returning, automatically restores previous scroll position
- **Easy Navigation**: Quick location when many folders present

### 5. Long Path Support
- **Auto-handling**: Automatically adds `\\?\` prefix when path exceeds 260 characters
- **Detailed Error Messages**:
  - File not found
  - Path not found
  - Access denied
  - Path too long (shows character count and suggestions)
- **Break Limitations**: Supports Windows long paths (exceeding MAX_PATH limit)

### 6. Progress Bar Seek Optimization
- **Precise Seeking**:
  - Resets `accumulatedTime` to target time
  - Resets high-precision timer `lastPerfCounter`
  - Recalculates playback time offset
- **Note Rebuilding**: Calls `RebuildActiveNotesAfterSeek()` to restore notes that should be playing
- **No Delay Response**: Immediate response, no burst of notes or no response

### 7. Track Switch Optimization
- **Clear Residual Keys**: Resets all piano key UI states when switching tracks
- **Avoid Conflicts**: Clears active notes and drum states
- **Chip Reset**: Automatically resets YM2163 chips to eliminate residual sounds

### 8. Dynamic Volume Mapping
- **Automatic MIDI Analysis**: Analyzes velocity distribution when loading MIDI files
- **Intelligent Mapping**:
  - Most common velocities → -6dB and -12dB
  - Peak velocities (95th percentile) → 0dB
  - Very low velocities → Mute
- **Adaptive Control**: Provides better volume balance for different MIDI files

### 9. Unicode Filename Support
- **CJK Font Support**: Full support for Chinese, Japanese, Korean characters
- **Font Merging**: Combines multiple fonts for complete character coverage
  - Primary: Microsoft YaHei (Chinese)
  - Secondary: Malgun Gothic (Korean)
  - Tertiary: MS Gothic (Japanese)
- **UTF-8 Handling**: Proper string handling throughout application

### 10. Win11-Style Address Bar
- **Breadcrumb Navigation**: Click directory buttons to navigate
- **Smart Truncation**: Shows "..." for long paths, prioritizes current directory
- **Folder Name Abbreviation**: Long folder names auto-abbreviated to "1234...7890" format
- **Dual Mode**:
  - Breadcrumb mode (default): Click buttons to navigate
  - Text input mode: Double-click to enter path directly
- **Up Arrow Button**: Quick navigation to parent directory

### 11. File Browser Improvements
- **Single-Click Operation**: Unified single-click for play and select
- **History Sorting**: Most recently opened directories appear first
- **Auto-reordering**: Revisiting directory moves it to top
- **Persistent Storage**: Saved to ym2163_folder_history.ini

### 12. Time-Driven MIDI Progress
- **High-Precision Timing**: Uses QueryPerformanceCounter (microsecond accuracy)
- **Smooth Animation**: Progress bar moves smoothly without jumps
- **Accurate Display**: Shows MM:SS format for current and total time
- **Drag Support**: Drag progress bar to quickly jump to any position

---

## 🐛 Bug Fixes

### Playback Related
- ✅ Fixed progress bar seek causing no response or burst of notes
- ✅ Fixed UI piano keys remaining pressed when switching tracks during sustained notes
- ✅ Fixed missing chip reset when double-clicking to load files

### UI Related
- ✅ Fixed drum UI lighting up on both chips simultaneously
- ✅ Fixed exited folder highlight flashing briefly
- ✅ Fixed scroll position not being recorded and restored
- ✅ Fixed layout width changes when toggling Slot1
- ✅ Fixed address bar focus loss issues

### File Handling
- ✅ Fixed Unicode character display issues
- ✅ Fixed long path files unable to load
- ✅ Improved error message clarity

---

## 🔧 Technical Details

### Dynamic Drum Allocation Implementation
```cpp
void play_drum(uint8_t rhythm_bit) {
    int chipIndex = 0;
    if (g_enableSecondYM2163) {
        chipIndex = g_currentDrumChip;
        g_currentDrumChip = 1 - g_currentDrumChip;  // Alternate
    }
    write_melody_cmd_chip(0x90, chipIndex);
    write_melody_cmd_chip(rhythm_bit, chipIndex);

    // Track drum state separately for each chip
    g_drumActive[chipIndex][i] = true;
}
```

### Progress Bar Seek Fix
```cpp
// Reset high-precision timer
QueryPerformanceCounter(&g_midiPlayer.lastPerfCounter);

// Recalculate accumulated time
double ticksPerMicrosecond = ticksPerQuarterNote / tempo;
g_midiPlayer.accumulatedTime = targetMidiTick / ticksPerMicrosecond;

// Rebuild note state
RebuildActiveNotesAfterSeek(targetEventIndex);
```

### Filename Scroll Animation
```cpp
struct TextScrollState {
    float scrollOffset;        // Current scroll offset
    float scrollDirection;     // 1.0=right, -1.0=left
    float pauseTimer;          // Pause timer
    std::chrono::steady_clock::time_point lastUpdateTime;
};

// Scroll speed: 30 pixels/second
// Pause duration: 1 second
```

### Long Path Support
```cpp
// Add long path prefix when path exceeds 260 characters
if (wFilename.length() > 260) {
    wFilename = L"\\\\?\\" + wFilename;
}
```

### Dynamic Volume Mapping Analysis
```
Analysis includes:
- Total note count
- Velocity range (min/max)
- Average velocity
- Peak velocity (95th percentile)
- Most common velocities (top 2)
- Calculated thresholds for each volume level
```

### Font Loading Strategy
```
Priority order:
1. Microsoft YaHei (msyh.ttc) - CJK support
2. Malgun Gothic (malgun.ttf) - Korean support
3. MS Gothic (msgothic.ttc) - Japanese support
4. Segoe UI (segoeui.ttf) - Western support
5. ImGui default - Fallback
```

### Chip Reset Sequence
```
For each chip:
1. Send note-off for all 4 channels
2. Set all volumes to mute
3. Reset all envelopes to decay
4. Reset all wave/timbre to 0
5. Turn off all rhythm sounds
6. Wait 50ms for stabilization
```

---

## 📊 Performance Improvements

- Reduced CPU usage during MIDI playback
- Improved font rendering performance
- Optimized file browser responsiveness
- Better memory management for large MIDI files
- High-precision timer reduces latency

---

## ⚠️ Known Limitations

- Maximum 20 MIDI folder history entries
- Single MIDI file playback (no playlist)
- Limited to 8 channels (4 per chip)
- Requires SPFM hardware for audio output
- Windows MAX_PATH limit (260 chars) overcome with long path support

---

## 💻 System Requirements

- Windows 10/11 (64-bit)
- SPFM hardware with YM2163 chip(s)
- FTDI USB driver
- 100MB free disk space

---

## 📦 Installation Instructions

1. Extract YM2163_Piano_v9_Release.tar.gz
2. Run ym2163_piano_gui_v9.exe
3. Ensure ym2163_midi_config.ini is present
4. Connect SPFM hardware via USB

---

## 🔨 Building from Source

```bash
cd YM2163_Piano_v9_Release
./build_v9.sh
```

---

## 📝 Changelog from v8

### New Features
- ✨ Dynamic drum allocation system
- ✨ File browser highlight system (exited folder, playing path)
- ✨ Filename scrolling display
- ✨ Scroll position memory
- ✨ Long path support
- ✨ Dynamic volume mapping
- ✨ Win11-style address bar
- ✨ Folder name auto-abbreviation

### Improvements
- 🔧 Progress bar seek optimization
- 🔧 Track switch optimization
- 🔧 Enhanced Unicode filename support
- 🔧 Improved history directory sorting
- 🔧 Unified single-click file browser operation
- 🔧 Confirmed time-driven MIDI progress

### Bug Fixes
- 🐛 Fixed drum UI display issue
- 🐛 Fixed progress bar seek issue
- 🐛 Fixed piano key residual issue
- 🐛 Fixed folder highlight issue
- 🐛 Fixed scroll position issue
- 🐛 Fixed long path loading issue
- 🐛 Fixed multiple UI layout issues
- 🐛 Improved overall stability

---

## 🗺️ Future Roadmap

- Playlist support
- MIDI file editing capabilities
- Advanced frequency tuning interface
- Real-time spectrum analyzer
- Recording capabilities
- More chip support

---

## 📞 Support & Feedback

For bug reports and feature requests, please visit the project repository.

---

## 🙏 Credits

- YM2163 chip documentation
- SPFM hardware support
- ImGui framework
- MidiFile library
- FTDI driver support
- All testers for their feedback

---

## 📄 License

This project follows open source license. See LICENSE file for details.

---

**Version History**: v1.0 → v2.0 → v3.0 → v4.0 → v5.0 → v6.0 → v7.0 → v8.0 → **v9.0**

**Last Updated**: January 28, 2026 23:53
