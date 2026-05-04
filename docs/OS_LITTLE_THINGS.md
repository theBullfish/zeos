# Things a real OS needs that nobody thinks of until they're missing

Backlog of small/peripheral capabilities. Most of these aren't features — they're
the texture of feeling like a finished system. A user notices their absence, not
their presence. Grouped to make it scannable; not exhaustive; append as we find
more.

Status legend: `·` not started · `~` partial · `✓` done

---

## Time, date, locale

- · Real-time clock readback (CMOS RTC) — boot-time wall clock
- · NTP client (or any time sync) — drift correction
- · Monotonic clock that survives suspend/resume — separate from wall clock
- · Timezone database (IANA tzdata) — DST rules
- · "Show seconds in clock" toggle
- · 12h vs 24h time format
- · Date format (M/D/Y vs D/M/Y vs Y-M-D ISO)
- · First day of week (Sun vs Mon)
- · Locale-aware number formatting (1,234.56 vs 1.234,56 vs 1 234,56)
- · Currency symbol + position
- · Temperature unit (C/F)
- · Speed/distance unit (mph/kph, mi/km, ft/m)
- · Paper size default (Letter vs A4)
- · Calendar systems beyond Gregorian (Hebrew, Hijri, etc.)
- · Right-to-left language layout
- · Keyboard layout switcher (US/UK/DE/FR/...)
- · Input method (IME) for CJK
- · Dead keys / compose key
- · Unicode normalization in filenames

## Boot, shutdown, recovery

- · Splash screen during boot (current path is text logs)
- · Boot menu (chain-load other OS, recovery shell, safe mode)
- · "Did not shut down cleanly" warning + fsck on next boot
- · Recovery partition / safe mode
- · Factory reset path
- · Install media generator
- · Rescue shell with minimal drivers when main boot fails
- · Boot performance trace (where did the 8s go?)
- · Last-known-good config restore
- · Single-user / maintenance mode
- · Welcome wizard on first boot
- · Reboot countdown with cancel
- · Logout confirmation on unsaved work
- · Force restart (hold power button — kernel registers this)
- · Wake-on-LAN
- · Wake-on-USB
- · Wake-on-RTC alarm
- · Power button behavior config (suspend vs shut down vs ask)
- · Lid close behavior config
- · Boot-up chime / login sound
- · Crash recovery — restore session after panic

## Power, thermal, battery

- · Battery percent + remaining time estimate
- · Battery health (cycle count, design vs full capacity)
- · AC adapter detect (charging / not charging / not present)
- · Power profile (performance / balanced / power saver)
- · Per-AC-state profiles (different defaults plugged in vs on battery)
- · Lid state (open / closed)
- · Display brightness slider + keyboard-shortcut control
- · Display brightness auto-adjust (ambient light sensor)
- · Display dim on idle
- · Display sleep timeout
- · System sleep timeout
- · Hibernate to disk
- · Schedule wake (alarm, calendar event)
- · Battery low notification + auto-suspend at critical
- · Thermal throttle indication (UI says why machine is slow)
- · Fan curve / quiet mode
- · CPU governor (performance / ondemand / powersave)
- · GPU power state visibility
- · "Power-hungry app" notification (this is what drained your battery)

## Input

- · Function keys (volume up/down, brightness up/down, mute, play/pause, prev/next, mic mute, airplane mode, search, screenshot)
- · FN-lock toggle (F-keys vs media keys default)
- · Touchpad: tap to click, two-finger scroll, three-finger swipe, pinch zoom, palm rejection
- · Touchpad gesture customization
- · Mouse: scroll speed, acceleration, primary-button swap (left-handed)
- · Mouse polling rate
- · Side buttons (back/forward)
- · Trackpoint (red nub) support
- · Touch screen + multi-touch
- · Stylus / pen + tilt + pressure
- · Game controller (Xbox/PS/Switch via USB or Bluetooth)
- · Force feedback / rumble
- · Sticky keys (accessibility)
- · Slow keys (accessibility)
- · Bounce keys (accessibility)
- · Mouse keys (drive cursor with numpad)
- · Caps Lock / Num Lock / Scroll Lock state indicators
- · Click sounds for key feedback
- · Key repeat rate + delay
- · Insert key behavior (overwrite mode)
- · Print Screen → screenshot
- · Hardware media keys mapping per-app

## Audio

- · Master volume + per-app volume
- · Default output device + per-app override
- · Default input device (mic) + mic mute
- · Headphone auto-detect / auto-switch
- · HDMI/DP audio auto-route on monitor connect
- · Bluetooth audio routing
- · Sample rate auto-negotiate
- · 24-bit / 32-bit float pipeline
- · Mono toggle (accessibility)
- · Audio balance L/R
- · Equalizer / EQ presets
- · Spatial / surround
- · System sounds (notification / error / startup)
- · Sound theme picker
- · Mic boost / noise suppression / echo cancel
- · Hardware mic mute LED honor
- · Camera/mic privacy indicator (LED next to in-use)

## Display

- · Resolution picker per output
- · Refresh rate picker per output
- · UI scale (100/125/150/200%) per output (different scales per monitor)
- · Mixed-DPI handling across monitors
- · Display arrangement / mirroring
- · Primary display selection
- · Rotation (0/90/180/270)
- · Night light / blue-light filter (color temperature shift on schedule)
- · True Tone / ambient color adapt
- · Gamma / brightness / contrast / saturation per display
- · ICC color profile per display
- · HDR toggle + tone-mapping
- · Color blind filter (deuteranopia / protanopia / tritanopia)
- · Cursor: size, color, blink rate, accent ring
- · Hardware cursor vs software cursor
- · Screensaver
- · Screen lock on idle
- · Screen lock on lid close
- · "Show clock on lock screen"
- · Screen recording (with mic / cursor / clicks)
- · Screenshot (region / window / full / delayed / clipboard / file)

## Networking edge

- · WiFi network list + signal strength + security type
- · WiFi password vault
- · Auto-connect priority order
- · Captive portal detection
- · Hidden network manual add
- · WiFi calling / hotspot mode (this device IS the AP)
- · Bluetooth tethering
- · USB tethering
- · VPN (WireGuard, OpenVPN, IKEv2)
- · Proxy config (HTTP / SOCKS / PAC)
- · /etc/hosts equivalent
- · DNS-over-TLS / DNS-over-HTTPS
- · mDNS / Bonjour for local discovery
- · NetBIOS / SMB browse
- · Hostname configuration
- · Network usage / data cap awareness
- · Metered connection flag (don't auto-update on cellular)
- · Network priority (Ethernet > WiFi > Cellular)
- · Airplane mode toggle
- · Connectivity test (am I really online?)

## Printing, scanning

- · Printer discovery (mDNS / SNMP / IPP)
- · Print queue per printer
- · Cancel print job
- · Print preview
- · Print to PDF
- · Print to file
- · Paper size, orientation, duplex, margins, scale
- · Scanner support (TWAIN / SANE-equivalent)

## Bluetooth

- · Pair / unpair
- · Trust / untrust
- · Auto-reconnect on wake
- · Battery level for connected peripherals
- · Audio codec negotiation (SBC / AAC / aptX / LDAC)
- · LE / Classic both supported
- · Profiles: HID, A2DP, HFP, MAP, PBAP, SPP, GATT

## USB / removables

- · Auto-mount on insert
- · Eject / safe remove
- · "Disk was not ejected properly" warning on yank
- · Format / partition tool
- · USB device authorization (security: ask before mount)
- · USB-C alt mode (DP, Thunderbolt) signaling
- · USB-C power role (source/sink negotiation)
- · USB-C cable orientation
- · Per-port power budget

## Files & filesystem

- · Trash / Recycle Bin
- · Trash auto-empty schedule
- · Undo move / undo delete
- · Hidden files (dotfiles)
- · Symbolic links
- · Hard links
- · File associations (default app per extension / mime type)
- · "Open With..." submenu
- · MIME type sniffing (don't trust extensions)
- · Quick Look / file preview without opening
- · Thumbnails for images / videos / PDFs
- · Recently opened files (per app + system-wide)
- · Pinned / favorite locations
- · File copy progress + cancel
- · File copy that survives sleep / network drop / disk-full
- · "Are you sure?" on overwrite
- · Move-to-trash vs delete-permanently distinction
- · fsck / disk repair tool
- · Disk usage analyzer
- · Defrag (irrelevant on SSD; relevant on HDD/SMR)
- · Quotas per user
- · Encrypted home / encrypted volumes
- · External drive encryption interop
- · Snapshot per filesystem
- · File watch / inotify equivalent
- · Search index (full-text + metadata)

## Process / app lifecycle

- · Process list (Activity Monitor / Task Manager)
- · CPU / memory / disk / network per process
- · Force quit / SIGKILL UI
- · App background activity manager (battery hog warnings)
- · App permissions (camera / mic / location / files / network)
- · App sandbox
- · App install / uninstall
- · App update channel + rollback
- · App signing / verification
- · Auto-launch at login config
- · Background services config
- · "Open at login" per app
- · Crash dump per process + viewer
- · "App not responding" detection + force-restart suggestion

## Notifications

- · Toast / banner system
- · Notification center (history / replay / clear)
- · Per-app notification settings (sound, banner, badge, lock screen)
- · Do Not Disturb
- · DND schedule (auto bedtime mode)
- · Focus modes (work / personal / gaming)
- · Action buttons in notifications
- · Reply inline from notification
- · Urgency levels (info / warn / critical)
- · Critical alerts that bypass DND (alarm, low battery)
- · Notification grouping per app
- · Persistent badge counts on dock/launcher

## Window management

- · Move / resize / minimize / maximize / close
- · Snap to half / quadrant
- · Snap groups (windows that resize together)
- · Virtual desktops / spaces
- · Per-monitor virtual desktops
- · Window picker (Alt-Tab + Mission Control)
- · Always-on-top
- · Sticky windows (visible on all desktops)
- · Window translucency / blur
- · Shadow / outline rendering
- · Focus follows mouse vs click-to-focus
- · "Bring all windows of app forward"
- · Cycle windows of one app
- · Tabbed windows (system-level)
- · Picture-in-picture for video

## Search

- · Spotlight-equivalent global launcher
- · App search
- · File / content search
- · Calculator inline (`12*4` returns 48)
- · Currency / unit conversion inline
- · Dictionary / definition lookup
- · Web suggestions (opt-in)
- · Recent commands history

## Clipboard, drag-and-drop

- · System clipboard
- · Multiple format handling (rich + plain text)
- · Image / file copy
- · Clipboard history (last N)
- · Sync clipboard across devices (handoff)
- · Drag-and-drop between apps
- · Drag-and-drop with hover-to-reveal (drag over folder, folder opens)
- · Drag preview thumbnail

## Text input gloss

- · Spell check
- · Grammar check
- · Auto-correct
- · Auto-capitalize
- · Smart quotes / dashes
- · Emoji picker
- · Character map / special characters
- · Dictation / voice input
- · Text replacement / snippets
- · Inline definitions
- · Find / replace in any text field
- · Undo / redo deep history
- · "Save As" dialog with recents
- · Recent locations in file dialogs

## Accessibility

- · Screen reader
- · Magnifier / zoom
- · High contrast theme
- · Reduce motion
- · Reduce transparency
- · Larger text everywhere
- · Voice control
- · Switch control (single-button input device)
- · Closed captions for system audio
- · Audio descriptions for video
- · Caption styling
- · Mono audio
- · Hover-text
- · Keyboard-only navigation everywhere
- · Tab focus indicators

## Security & privacy

- · Login / lock screen
- · Multi-user accounts
- · Fast user switching
- · Guest account
- · Parental controls (time limits, content filter)
- · Sudo / privilege elevation prompt
- · Disk encryption at rest
- · Per-app permissions (camera / mic / location / contacts / files)
- · "App is using your camera" indicator
- · "Recently used location" log
- · Firewall (incoming + outgoing)
- · TPM / secure element integration
- · Measured boot / attestation
- · Code signing
- · ASLR / NX / stack canaries / KPTI
- · Update channel verification
- · Fingerprint / face login
- · "Find My Device" + remote wipe

## Crash & debug

- · Kernel panic screen (not just dead silence)
- · Crash dump persistence across reboot
- · Crash report submitter (opt-in)
- · Boot logs persisted across reboots (so you can see why last boot died)
- · Log levels (debug / info / warn / error / fatal)
- · Log rotation
- · Per-subsystem log filtering
- · Live log viewer (`journalctl -f` equivalent)
- · Performance counters (perf-equivalent)
- · System call tracer (strace-equivalent)
- · Memory leak detector
- · Hardware error reporting (MCE / WHEA)
- · POST beep code interpretation
- · LED status meaning glossary

## Updates & maintenance

- · Update notifier
- · Update download in background
- · Update installer with rollback
- · Driver / firmware updates
- · Kernel updates with reboot
- · Update channel selector (stable / beta / dev)
- · "Restart later" + scheduled install
- · Notify-only mode (don't auto-install)
- · Storage pre-flight (enough disk for update?)
- · Update over metered connection guard

## Backup & sync

- · Time-Machine-equivalent (versioned backup)
- · Snapshot scheduling
- · External drive auto-backup on connect
- · Cloud sync (whose cloud? per-OS choice)
- · Conflict resolution UI
- · Restore from backup wizard
- · Migration from another machine

## Help & onboarding

- · `--help` everywhere
- · Tooltip on hover
- · Help center / documentation viewer
- · Man-page equivalent
- · Keyboard shortcut cheat sheet per app
- · Tour / guided intro on first launch
- · "What's New" panel after upgrade
- · In-app feedback / bug report
- · Self-test menu

## Telemetry

- · Opt-in usage metrics
- · Opt-in crash reports
- · "Why are you uninstalling?" survey
- · Performance regression beacon (sealed in user privacy)
- · Battery health telemetry (with consent)

## Multimedia

- · Image viewer (zoom, rotate, flip, crop, EXIF)
- · Video player (codecs, scrub, speed, captions, AB-loop)
- · Audio player (queue, EQ, gapless, ReplayGain)
- · Camera app
- · Voice recorder
- · QR code reader / generator
- · Color picker (eyedropper)
- · Screen ruler / pixel measure
- · Hardware video decode/encode (VA-API equivalent)
- · GPU-accelerated paint

## Fonts

- · Font fallback for missing glyphs (CJK / emoji / math)
- · Font installer
- · Font preview / specimen
- · Font hinting (full / slight / none)
- · Subpixel rendering toggle
- · Variable font support
- · Color font / emoji font
- · Per-app font override
- · System default font picker

## Themes & cosmetics

- · Light mode / Dark mode / Auto-by-time
- · Accent color picker
- · Wallpaper per virtual desktop
- · Wallpaper slideshow
- · Icon pack
- · Sound theme
- · Cursor theme
- · Login screen background
- · Lock screen widgets

## Calendaring & time

- · Alarm clock
- · Timer
- · Stopwatch
- · World clocks
- · Calendar app
- · Reminder / to-do
- · Meeting "do not interrupt" auto-DND

## Tiny dialogs that always exist

- · "Do you want to save changes?" before close
- · "Replace existing?" on overwrite
- · "Permission required: …"
- · "Network is offline" friendly state
- · "Disk is full" + offer to clean up
- · "Battery critically low — sleeping in 30s"
- · "Update available — install now / later / details"
- · "Are you sure you want to empty trash? N items, X MB"
- · "Force quit unresponsive app?"
- · "This action cannot be undone"

## "Power user" surfaces a real OS has

- · Cron / at / scheduled tasks UI
- · Service / daemon manager
- · Environment variables editor
- · Network interface manager
- · Process priority / nice
- · Disk partition tool
- · Boot loader / kernel parameter editor
- · Hex editor
- · Diff / merge tool
- · Calculator (programmer / scientific / statistics modes)
- · Activity log / audit trail

## Hardware oddities

- · Smart card reader
- · Fingerprint sensor
- · IR receiver / blaster
- · NFC reader
- · CEC over HDMI (control TV from PC, control PC from TV)
- · DisplayLink / USB displays
- · Thunderbolt eGPU detect + UI
- · Optical drive (CD/DVD/BD) — yes, still real
- · Floppy / tape (legacy industrial)
- · Serial console / RS-232 / RS-485
- · Parallel port (legacy printers / industrial)
- · GPIO header (Raspberry-Pi-style boards)
- · I²C / SPI device shells

## Network discovery / sharing

- · "Other devices on this network"
- · File sharing (SMB / NFS / AFP / WebDAV)
- · Screen sharing / remote desktop
- · AirDrop / LocalSend equivalent
- · Bonjour / mDNS service browser
- · Casting (AirPlay / Cast / Miracast)

## Things that fail unless you remember them

- · ESC closes a modal
- · Cmd-Z / Ctrl-Z works in EVERY text field
- · Tab moves focus in a sensible order
- · Enter activates the default button
- · Right-click works EVERYWHERE
- · Double-click distance + speed config
- · Hover delay before tooltip
- · Triple-click selects line
- · Quadruple-click selects paragraph
- · Drag with shift = constrain
- · Drag with option = duplicate
- · Drag with cmd = move-not-copy override
- · Modifier-key-only chords (just shift, just option)
- · Two-finger right-click on touchpad
- · Pinch to zoom in any image / map / document
- · Smooth scrolling vs line-by-line
- · Inertia scroll
- · Overscroll bounce
- · Scrollbar appears on hover only
- · Page Up / Page Down / Home / End correctness
- · Selection survives across Tab presses
- · Find-as-you-type in lists
- · Type-ahead in dropdowns (press "M" to jump to first M)
- · Click-and-hold to repeat (volume buttons)
- · Long-press for context menu (touch)

## Things people only notice when broken

- · Hover state on every clickable thing
- · Click-down state distinct from click-up
- · Disabled state clearly disabled (not just grey)
- · Focus ring on keyboard-focused element
- · Empty state (no items? show a friendly message, not a blank panel)
- · Loading state (spinner, skeleton, progress)
- · Error state with action to retry
- · Optimistic UI (commit to local before server confirms)
- · Undo for destructive actions (delete email, mark read, archive)
- · "Discard changes?" on close-with-unsaved
- · Form field validation as you type, not on submit
- · Field hints / placeholder text
- · Required vs optional clearly marked
- · Password show/hide toggle
- · Strength indicator on password
- · Confetti or celebration moment on success (small but present)

---

## What we already have in Zeos

- ✓ Boot logs printed to serial
- ✓ Selftest at boot
- ✓ Test framework (`tests` shell command)
- ✓ MasQ journal (chain provenance)
- ✓ B3 belief (per-chain failure tracking)
- ✓ Chain dump (`chains` / `inspect`)
- ✓ Vault (persistence primitive)
- ✓ HTTPS via TLS 1.3
- ✓ FAT32 read (write in flight)
- ✓ Multi-monitor chain shape
- ✓ Hub recursion to 5 tiers
- ✓ Display picker via GOP
- ✓ EDID parse
- ✓ HDA audio (post-codec-walk fix)
- ✓ Z+ language with chain binding
- ✓ Scheduler chain-resolution kernel

## What's high-leverage from the list above (top 20 to land first)

These are the ones whose absence makes Zeos feel unfinished even after every
weakness in the technical list is fixed.

1. RTC readback — wall-clock time
2. Login + lock screen
3. Notification system (toast + history + DND)
4. Volume controls + mute (with hardware key support)
5. Brightness controls (with hardware key support)
6. Battery percent + AC detect
7. Splash on boot (instead of text scroll)
8. Trash / Recycle bin
9. Cmd-Z / undo in any editable text
10. Right-click context menus everywhere
11. Hover state on clickables
12. "Save changes?" before close
13. Quick file preview (Quick Look)
14. Settings panel (single place to find all configs)
15. WiFi network list UI
16. Bluetooth pair UI
17. Power button behavior config
18. Lid close behavior config
19. Screensaver / screen lock on idle
20. Empty / loading / error states drawn for every list

These are not weaknesses — they're the texture of feeling like an OS instead of
a kernel demo. We knock these down after the technical weakness list closes.
