# YM2163 Piano GUI v9 - Quick Start Guide

## Installation
1. Extract the archive to your desired location
2. Ensure `ym2163_midi_config.ini` is in the same directory as the executable
3. Connect your SPFM hardware via USB
4. Run `ym2163_piano_gui_v9.exe`

## First Run
- The application will initialize the FTDI driver and YM2163 chip(s)
- Check the log window for initialization messages
- If you see "YM2163 initialized successfully", you're ready to go

## Playing MIDI Files
1. **Browse for MIDI files**:
   - Use the file browser on the left side
   - Navigate to folders containing MIDI files
   - Recently opened folders appear at the top of the history

2. **Load and Play**:
   - Double-click a MIDI file to load and play
   - Or click "Play" button after selecting a file
   - Use "Next >>" and "<< Prev" buttons to navigate tracks

3. **Control Playback**:
   - **Play/Pause**: Click the Play button or press Media Play key
   - **Stop**: Click the Stop button
   - **Seek**: Drag the progress bar to jump to any position
   - **Volume**: Use Volume +/- buttons or arrow keys

## Address Bar Navigation
- **Breadcrumb Mode**: Click directory buttons to navigate
- **Text Input Mode**: Double-click the address bar to enter a path directly
- **Up Arrow**: Click "^" to go to parent directory
- **Ellipsis**: Click "..." to see the full path

## Configuration
Edit `ym2163_midi_config.ini` to customize:
- Instrument settings (timbre, envelope, volume)
- Frequency mappings for each note
- MIDI channel assignments

## Keyboard Shortcuts
| Key | Action |
|-----|--------|
| Media Play/Pause | Play/Pause |
| Media Next | Next Track |
| Media Previous | Previous Track |
| Page Up | Octave Up |
| Page Down | Octave Down |
| Up Arrow | Volume Up |
| Down Arrow | Volume Down |

## Features Overview

### Dynamic Volume Mapping
- Automatically analyzes MIDI file velocity distribution
- Adapts volume levels for optimal sound balance
- Check the log for velocity analysis details

### Dual Chip Support
- Enable "Enable 2nd YM2163 (Slot1)" for 8 channels total
- Default: 4 channels (Slot0 only)

### Sustain Pedal
- Enable "Sustain Pedal" to map CC64 to envelope control
- Pedal down = Fast envelope, Pedal up = Decay envelope

### Auto-Skip Silence
- Enable "Auto-Skip Silence" to jump to the first note
- Useful for MIDI files with long silence at the start

### Global Media Keys
- Enable "Global Media Keys" to control playback from anywhere
- Works even when the window is not focused

## Troubleshooting

### No Sound Output
- Check SPFM hardware connection
- Verify FTDI driver is installed
- Check log for initialization errors
- Try re-initializing the chip (toggle Slot1 or restart)

### Korean/Japanese Characters Show as "????"
- Ensure system fonts are installed (Microsoft YaHei, Malgun Gothic, MS Gothic)
- Restart the application
- Check Windows font settings

### Progress Bar Jumps
- Verify MIDI file format compatibility
- Try a different MIDI file
- Check for tempo changes in the MIDI file

### MIDI Playback Issues
- Verify MIDI file format (should be standard MIDI)
- Check MIDI channel assignments in config
- Try adjusting velocity mapping settings

## Tips & Tricks

1. **Organize MIDI Files**: Keep MIDI files in organized folders - they'll appear in history for quick access

2. **Customize Instruments**: Edit `ym2163_midi_config.ini` to create custom instrument presets

3. **Monitor Velocity Analysis**: Check the log window when loading MIDI files to see velocity distribution analysis

4. **Use Keyboard Shortcuts**: Media keys work globally for convenient playback control

5. **Dual Chip Mode**: Enable Slot1 for more polyphony with complex MIDI files

## Next Steps
- Explore the MIDI folder history to quickly access frequently used folders
- Experiment with different instrument settings in the config file
- Try enabling/disabling features to find your preferred setup
- Check the log window for detailed information about playback and chip status

## Support
For issues or feature requests, refer to the project documentation or repository.
