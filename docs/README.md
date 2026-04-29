# YM2163 Piano GUI v9 Release

## Overview
YM2163 Piano GUI v9 is an advanced MIDI player and synthesizer interface for the YM2163 sound chip via SPFM hardware.

## New Features in v9

### 1. Dynamic Volume Mapping
- Automatically analyzes MIDI file velocity distribution
- Maps most common velocities to -6dB and -12dB
- Maps peak velocities to 0dB
- Maps very low velocities to Mute
- Provides adaptive volume control for better sound balance

### 2. Unicode File Name Support
- Full support for Korean (한글), Japanese (日本語), and Chinese (中文) file names
- Font merging technology for CJK character display
- Proper UTF-8 handling throughout the application

### 3. Win11-Style Address Bar
- Breadcrumb navigation with clickable directory buttons
- Automatic truncation with "..." for long paths
- Double-click to enter text input mode
- Up arrow button for quick parent directory navigation

### 4. Improved File Browser
- Accurate button width calculation prevents text overflow
- Smart address bar layout prioritizes rightmost (current) directory
- Proper UTF-8 string handling for international file names

### 5. Chip Reset on Track Switch
- Automatic YM2163 chip reset when switching tracks
- Eliminates residual sound and lingering notes
- Works with both single and dual chip configurations

### 6. History Directory Sorting
- Most recently opened directories appear at the top
- Automatic reordering when revisiting directories
- Keeps up to 20 most recent MIDI folders

### 7. Time-Driven MIDI Progress
- High-precision timer-based playback (microsecond accuracy)
- Smooth progress bar animation
- Accurate time display (MM:SS format)
- Supports drag-to-seek functionality

## System Requirements
- Windows 10/11
- SPFM hardware with YM2163 chip(s)
- FTDI USB driver support

## Installation
1. Extract the archive
2. Run `ym2163_piano_gui_v9.exe`
3. Ensure `ym2163_midi_config.ini` is in the same directory
4. Connect SPFM hardware via USB

## Building from Source
```bash
./build_v9.sh
```

## Configuration
Edit `ym2163_midi_config.ini` to customize:
- Instrument settings (timbre, envelope, volume)
- Frequency mappings
- MIDI channel assignments

## Features
- Real-time MIDI playback with YM2163 synthesis
- Dual chip support (8 channels total)
- Dynamic velocity mapping
- Sustain pedal support
- Global media key support
- Auto-skip silence at track start
- Sequential and random playback modes
- Comprehensive MIDI folder history

## Keyboard Shortcuts
- Play/Pause: Media Play/Pause key
- Next Track: Media Next key
- Previous Track: Media Previous key
- Octave Up: Page Up
- Octave Down: Page Down
- Volume Up: Up Arrow
- Volume Down: Down Arrow

## File Browser
- Double-click MIDI files to play
- Click directory buttons to navigate
- Click "..." to view full path
- Use address bar for quick navigation

## Troubleshooting
- If Korean/Japanese characters show as "????", ensure system fonts are installed
- If MIDI playback is silent, check SPFM hardware connection
- If progress bar jumps, verify MIDI file format compatibility

## Version History
- v9: Dynamic volume mapping, Unicode support, improved UI
- v8: Dual chip support, MIDI player enhancements
- v7: Initial release with core functionality

## License
See LICENSE file for details

## Support
For issues and feature requests, visit the project repository.
