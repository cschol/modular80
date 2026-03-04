### 2.0.3 (2026-03-03)

- Fix worker thread silence caused by uninitialised stopWorker atomic (Mark)
- Fix error log formatting on Windows platform (Mark)

### 2.0.2 (2026-02-14)

- Fix Logistiker reset flag bug that could cause delayed reset behavior
- Improve Logistiker reset logic to work independently of clock input
- Fix Nosering DAC coefficient precision for accurate voltage output scaling
- Fix Nosering analog input threshold comparison for more robust invert control
- Fix Nosering uninitialized variable
- Fix RadioMusic RAW audio file memory allocation order to prevent potential crashes
- Improve RadioMusic audio quality with 4-point 3rd-order optimal interpolation (Watte tri-linear)
- Improve RadioMusic crossfade logic with channel count validation to prevent audio artifacts
- Enhance RadioMusic thread safety with proper condition variable synchronization
- Code documentation improvements and error handling enhancements across all modules

### 2.0.1 (2022-01-07)

- Fix playback behavior in Radio Music to keep playing when station is changed (match hardware).

### 2.0.0 (2021-10-18)

- Update to Rack v2
- Add root directory location to context menu
- Add Pitch Mode (available via context menu)
- Add Stereo Mode option to allow stereo output (polyphonic cable) - default to Mono mode to preserve compatibility
- Add option to clear current bank and stop playback to context menu
- Change bank size limit from file size to memory limit (2GB in RAM per bank)
- Allow current pool of audio files in root directory to continue to play while new pool of audio files is loaded (by loading and background and swapping)
- Set different default values and labels for Start knob in Pitch mode
- Allow saving of current **bank** of audio files to be saved to Patch Storage.

### 1.0.3 (2021-05-13)

- Fix crash when loading RAW files

### 1.0.2 (2020-11-29)

- Fix WAV file playback

### 1.0.1 (2020-03-02)

- Relicense source code to GPLv3
- Update module description for Radio Music
- Add Hardware Clone tag

### 1.0.0 (2019-06-03)

- Initial release for Rack v1
