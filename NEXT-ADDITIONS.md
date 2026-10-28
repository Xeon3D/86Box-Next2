# 86Box-Next2: additions over upstream 86Box

Status as of **Release 3.0** (build 186, 18 October 2026; the first 86Box-Next2 release, new recompiler only). For each addition: what it is,
its status, and the known bugs or limitations. "Driver" or "hardware" means real hardware with
that driver behaves the same; those are not emulation bugs. Details live in `CLAUDE.md`, the
handoff files.

**Status legend:** ✅ working and verified in guests · 🟡 working, partly verified or with open
issues · 🧪 experimental / not tried in a guest · ⛔ not in this release (unmerged branch)

---

## Emulator and user interface

| Addition | Status | Known bugs / limitations |
|---|---|---|
| **86Box-Next2 branding** (`EMU_DISPLAY_NAME`; `EMU_NAME` stays "86Box" for paths and guest-visible IDs) | ✅ | None known. |
| **Per-machine config only** (no global config; `vmm.ini` beside `86box.cfg`) | ✅ | None known. |
| **VM Manager optional, off by default** | ✅ | None known. |
| **Build number and releases** (`.build-number` in the title bar, `NEXT_RELEASE=N` → "Release N", `tools/make-release.sh`) | ✅ | Releases are Qt 6 only. `make-release.sh` refuses to run while any 86Box-Next2 process runs, and stages branch-owned rigs too (the ISDN rig's exe must be kept aside). |
| **Status bar icon order** (Preferences > Status bar) | ✅ | A new icon group needs its key and name added by hand. The PC Card/USB/modem/Sound Canvas menus used to be lost when Preferences replaced the status bar; fixed (`adoptMenus()`). |
| **Soft settings changes** (PC Cards, COM/LPT devices, MIDI, ISA MPU-401 without a PC reset) | ✅ | Still need a hard reset: port enable/disable, ECP DMA jumper, MCA MPU-401, a port held by a non-hotplug device (serial mouse), fitting or removing the PCMCIA controller. A COM-port soft-reset crash was fixed (b33868148). |
| **BIOS vendor in Settings > Machine** | ✅ | Machines the generator cannot place show "Unknown"; rerun `tools/machine-bios-vendors.py` when upstream adds machines. |
| **Debug command file** (`BOX86NEXT_DEBUG_CMD`: keys, typing, mouse, screenshots, breakpoints, memory peeks, device dumps, CD swap, reset) | ✅ | `type` assumes a US keyboard layout (other layouts need scancodes). `interp 1` is cleared by every CPU reset. `bp` works in the interpreter only. Alt+Enter injection does not toggle full screen. |
| **Upstream CI disabled**, weekly `sync-upstream.yml` merge | ✅ | None known. |
| **Old recompiler removed** (86Box-Next2: upstream retired it; the new recompiler is the only one, as in upstream) | 🟡 new in 3.0 | Releases up to 2.1.5.1 were built with the old recompiler (`NEW_DYNAREC=OFF`); guests verified there have not all been rechecked on the new one. The fork's old-recompiler x87 EMMS fix is gone: upstream's own fix covers it. |

## Devices

| Addition | Status | Known bugs / limitations |
|---|---|---|
| **Arcade I/O boards** (funworld Photo Play; Merit Megatouch XL/MAXX) | ✅ | None known. |
| **Elo TouchSystems SmartSet serial touchscreen** | ✅ | None known. |
| **PCMCIA controller** (Cirrus CL-PD6722, two hot-plug sockets, status-bar PC Card menu) | ✅ Win98 | SRAM/flash memory cards were removed (2026-10-07). The Win98 image's USB-backport tray "Stop" does nothing: guest software, not the emulator. |
| ↳ 3Com 3C589D | ✅ | None known. |
| ↳ TRENDnet TE100-PC16 | ✅ | None known. |
| ↳ 3Com 3C562D LAN + 33.6 modem (multi-function) | ✅ Win98 | Windows tends to put the modem on COM4 at 2E8h, which S3 cards' 8514/A registers shadow (reads 00h, real-hardware behaviour): move the modem's I/O. ATI answers are invented; the LAN product ID is unchecked against a real card. |
| ↳ Hayes Accura 56K | 🧪 | Not tried in a guest; ATI answers are plausible, not dumped. |
| ↳ Adaptec APA-1460 SlimSCSI | ✅ Win98 | PORTA/B read FFh. |
| **USB** (UHCI VIA/Intel, EHCI + companion, host passthrough via libusb/UsbDk, connect prompt, trace) | ✅ | No siTDs: full-speed devices on the USB 2.0 card go through the companion controller. A crash when two VMs listed USB devices at once is fixed (7f62b62c1); older exes can still hit it. |
| ↳ PIIX3/PIIX4 onboard USB (function 2 as a working UHCI, with LEGSUP legacy keyboard support: 60h/64h traps that keep trapped cycles from the KBC, SMI on the USB interrupt, A20GATE pass-through; on the PIIX4 through GLBSTS bit 1) | 🟡 | USB keyboard in POST and BIOS setup on the ECS P5VX-B (Award 1.5c) and Intel SE440BX-2; after POST on the Biostar MB-8500TUC and MB-8500TVX-A, AOpen AP5VM and AP53, FIC KN-6000 and AIR P6KDI (AMI: they start USB after their DEL prompt; the KN-6000 and P6KDI need a Pentium II -- they never supported the Pentium Pro) and Intel VS440FX (USB Legacy is off by default); RadiSys EM440 at its F1 prompt. Not working: Intel AN430TX (its handler reads back the key it injects). No option: ASUS P/I-P55TVP4, PC Partner MB600N, Epox P55-VA; hidden in all three BIOSes: BCM FM530. All 61 PIIX4 boards were booted with a PS/2 keyboard: none breaks with the UHCI (eight fail the same way without it: six never start, the MS-6168 and RC440BX stay blank). A USB card on a PIIX3/PIIX4 board gets no ports. |
| ↳ USB on the other southbridges (VIA 586B-686B UHCIs, ALi M1543/C, SiS 5571/5582/5595, SMSC SLC90E66, STPC Atlas OHCIs: upstream's southbridge wiring over our UHCI and new OHCI; OPTi FireLink OHCI card; bus-time budget, EOF babble, debug mode, remote wakeup) | 🟡 | USB keyboard in BIOS setup (enumerated by the BIOS, Del and arrows) on the FIC VA-503+ (VIA 586B UHCI; "BIOS Support USB Keyboard") and ASUS P5A (ALi M5237 OHCI; "USB Function" in PnP/PCI setup), 2026-10-27. Not yet tried under an OS. The P5A's setup misses keys pressed and released within ~16 ms (every report reaches it: its own speed). |
| ↳ 4-port USB 1.1 hub (upstream's `usb_hub.c` on our bus, Settings > Other peripherals > Hub) | 🟡 | FIC VA-503+ BIOS enumerates the hub and the USB mouse behind it; not yet tried under an OS. Also a 4-port USB 2.0 hub (high speed with a TT on an EHCI port; EHCI split transactions and siTDs): Windows 2000 SP4 drives it at high speed with a USB mouse behind the TT. |
| **Settings layout**: Other peripherals tabs USB (up to four USB cards by free PCI slots) / PCMCIA / ISA / Arcade IO; Modems page (COM-port and internal modems, live state); Integration page (Guest Tools); Ports > USB as a tree with the hub chosen on a port | 🟡 | Looked at by the owner (2026-10-28); several USB cards not yet tried in a guest. |
| ↳ USB ports per controller, chosen in Settings > Ports > USB (keyboard, mouse, hub, host devices; red note on Input devices when the USB keyboard/mouse has no port; older configs migrated once) | 🟡 | Runtime checked (migration on the VA-503+, PIIX3 + USB 2.0 card in Windows 2000); the Settings tab itself not yet looked at by the owner. |
| ↳ USB keyboard and mouse ("USB Keyboard" / "USB Mouse" in Settings > Input devices, with a USB controller; separate low-speed HID boot devices 86B0:0001 and 86B0:0002, with the PS/2 layout and button options) | ✅ Win98 | Both together work in Windows 98 SE (P5VX-B rig). USB only: nothing works until the guest's USB driver or a BIOS with USB keyboard support drives them. Both together take both ports. |
| **COM-port modems** (Diamond SupraExpress 56e PRO, ELSA MicroLink 56k, Standard Hayes 56k; replaces upstream's network modem) | ✅ | The Hayes model has no voice; only the Supra has a Serial PnP ID. |
| **Internal modem** (Diamond SupraExpress 56i Sp, ISA PnP or jumpered) | 🧪 | No guest test recorded. |
| **Voice calls** (Rockwell `#` set and V.253, ADPCM, host handset/speaker/microphone) | ✅ | PC Card modems have no voice. |
| **isp-server** (submodule: virtual ISP + telephone exchange, web status page and browser phone; Windows/Linux/macOS/Docker) | ✅ | The browser phone's microphone needs a secure context (localhost or HTTPS). MS-CHAPv1 + MPPE can't be checked against Linux's pppd (pppd's own bug). |
| **Roland Sound Canvas MIDI out** (88emu, 24 boards, front panel; replaces MUNT and the CLAP device) | ✅ | Builds with it are GPLv3. Upstream changes to its CMake/`midi.h` blocks can conflict on sync. Needs the owner's ROM dumps. |
| **86Box Guest Tools** (Settings > Other peripherals; Tools > Install/Update 86Box Guest Tools: built-in CD/floppy; agents 86BOXGT.EXE for Windows 95/NT 4.0 and later, GTWIN.EXE for Windows 3.x; version 1.1.x): mouse integration without capture, keyboard follows the pointer between machine windows (without raising them), shared clipboard text, files through the clipboard and drag and drop both ways (host, guest, guest to guest) | 🟡 new in 2.1.5.1 | Files and drag and drop: Win32 agent only (Windows 95, 98 SE, NT 4.0, 2000 tested), not Windows 3.x. Up to 20000 entries / 2 GB per copy; files from a guest are copied to the host in the background as soon as the guest copies them, into `<data dir>/guest-tools/clipboard`. No DOS, Linux or OS/2 agents. A drag out of a guest waits at the drop for the files to arrive. |

## Video cards

| Card | Status | Verified | Known bugs / limitations |
|---|---|---|---|
| **NVIDIA RIVA 128 / 128 ZX** (NV3, PCI/AGP) | ✅ | Win98 SE (3DMark 99 ≈1465, DxDiag D3D, OpenGL screensavers), Win95, Win2000, NT4 AGP, WfW 3.11; mfgtest modes 33/33 | Filtering and alpha blending unchecked against real hardware. Stray columns in the overscan (hidden with overscan off). Overlay: no YV12/YVU9, no interlaced bob. Win2000 D3D is software only and OpenGL at 32 bpp uses Microsoft's renderer (driver limits). Two BIOS versions undumped. |
| **NVIDIA RIVA TNT / TNT2 / Ultra / M64** (NV4/NV5, 18 board BIOSes; CPU-thread or Vulkan 3D) | ✅ | Win98 SE (drivers 4.11–45.23: 3DMark 99/2000, UT, Quake III 39 fps, GLQuake 85, Quake II 38.5), Win2000, NT4 SP6a, DOS VESA | New in 2.1.5: GDI transparent hatches, quads, texel origin and lost-trap fixes. Open: TNT2 shows Win98's "safe to turn off" screen as 80×400 (suspected NV5 mode decode); at 8 bpp some DirectDraw checks fail (not triaged); glean blend test fails; D3D mipmap WARN. Not emulated: TV encoder, capture, class 0x48, 3D into swizzled surfaces. Driver limits: 71.84 draws no Direct3D; Alt+Tab from full-screen OpenGL crashes driver 4.11; 45.23 D3D needs VIAGART on VIA boards. |
| **S3 Savage3D** (STB Nitro 3200, AGP/PCI) | ✅ | Win98 SE (DxDiag 6.1, Tomb Raider 2 with S3TC/trilinear), NT4 (PCI card) | NT4 can't use the AGP card (NT4 HAL rejects it, as on real hardware): use the PCI card. Status-bit layout borrowed from the ViRGE, unverified. Not emulated: video capture, TV out. BIOS only in ROMS-EXTRA. |
| **3Dlabs Permedia 2** (STB GLyder MAX-2+, ELSA WINNER 2000/Office, GLoria Synergy) | 🟡 | Win98 SE (3Dlabs, ELSA, Synergy drivers: DxDiag DX7, 3DMark 99 2050, TR2, GLQuake), NT4, Win2000 inbox, DOS UniVBE | New in 2.1.5: 3D, YUV blits and rendering fixes. Later fixes: overlapping blits (3Dlabs drivers, Win2000 perm2), 3Dlabs colour brushes and text, and NT4/Win2000 lines and ellipses. **Open:** 3Dlabs Win98 DirectDraw source colour key (its HAL blits with the CPU), NT4 hatch alignment and first DIB brush, a Win2000 hatched rectangle with swapped colours, 8 bpp colour mismatches on NT4/Win2000. Win98 drivers' diagonal lines are off GIQ by their own setup (as on the chip). Hardware limits: no mipmapping, no alpha test, no scan-out overlay. Not modelled: patch address permutation, overlay filtering (nearest only), video capture/TV. 24 bpp untested (no driver offers it). |
| **S3 ViRGE/MX** (STB Symmetry 3D), generic **ViRGE/MX AGP**, **ViRGE/MX+** (Spectrah LD-S500) | ✅ | MX+ on Win95, Win98 SE, NT4 SP6a, Win2000 at 1024×768; GDI exact on Win98/2000/NT4 | New in 2.1.5: CRTC register pairing and 2D lines per the datasheet. Generic MX on Win95/98 shows only a 640×480 window of a larger desktop (S3 BIOS/driver display-switch state, not emulation; Win95 confirmation pending). The STB ROM holds its sign-on screen 20–25 s (the ROM). Not yet run: STB Symmetry 3D in Windows, DDTEST. |
| **Chips & Technologies 65554** (STB Monocle) | 🟡 | Win98 SE built-in driver | Without a panel it shows the CRTC timing, not the panel-cut picture. Interlaced modes halve vertical timings. The driver's 1024×600 entry fails. VBETest's 4-bit tests look wrong (VBETest's doing). |
| **Alliance ProMotion 6410 / 6422 / AT3D (AT25)** | 🟡 new in 2.1.5 | POST/VESA on all three; Win95, Win98 SE, NT4 with Alliance drivers; DirectDraw and Media Player overlay on the 6422; AT3D 2D, DirectDraw and NT 4, both video windows (2.1.5.1) | AT3D is 2D only (no 3D engine, no Voodoo Rush). The STB source build's DirectDraw HAL doesn't come up (cause unknown). Video window: horizontal interpolation only (no downscaling or TV flicker filter), interlace shown progressive, 24 bpp untested. ROMs only in ROMS-EXTRA. |
| **SpeedStar 24X** as a WD90C31A-LR BIOS option | 🧪 | — | Not booted in a guest yet. |

## Machines and chipsets

| Addition | Status | Known bugs / limitations |
|---|---|---|
| AGP chipset fixes (incl. SiS) | ✅ | None known. |

## Not in this release (unmerged branches)

| Branch | What | Status |
|---|---|---|
| `isdn-fritz` | AVM FRITZ!Card PnP ISDN adapter | ⛔ 🧪 passes only a host-side harness with AVM's driver library; not tried in a guest. Its rig (`AVM-ISDN-RIG`) keeps the branch build. |
| upstream-PR branches (`nv3-riva128-*`, `speedstar24x-*`, `txp4x-*`, `via-apollo-vpx-tests`, `agp-fixes`) | Work prepared for upstream submissions | ⛔ kept for reference. |
