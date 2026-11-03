# Notes for agents and contributors

Engineering notes for changing this code. The README is install-only; everything
about *why the code looks like this*, and every trap found getting it onto a phone,
lives here.

Written in English to match the source comments.

## What this is

A Windows 10 Mobile (UWP) internet radio player in C#/XAML. It is a **rewrite**,
not a port: the Linux and macOS builds share a Rust crate, and none of it can be
reused here.

| Layer | Why the Rust code cannot be reused |
|---|---|
| Rust std | `thumbv7a-uwp-windows-msvc` is a tier-3 target with no prebuilt std - needs nightly and `-Z build-std`. |
| `winit` (windowing, under egui/eframe) | No UWP backend at all: android, ios, linux, macos, orbital, web, windows - and "windows" means Win32 HWND, which does not exist on a phone. |
| `cpal` (audio output) | No UWP/WinRT backend; its Windows host is WASAPI for Win32. |

So the UI and audio output were written against WinRT. What *is* carried over,
translated to C#, is the part that was hard to get right:

- **`Services/StreamResolver.cs`** - HTTP redirect and playlist indirection
  following. Ported from the Rust original; the invariants in its header comment
  each exist because a real station broke without them.
- **`Services/StationCatalog.cs`** - tolerant dataset parsing (radio-browser
  sometimes sends a number as a string and vice versa; a corrupt country file is
  skipped, not fatal).
- **`Assets/data/`** - the station list. It is not a copy: the `.csproj` lists
  every file of the repository's `../shared/data` as `<Content>` with a `<Link>`
  under `Assets\data`, so the package layout is unchanged and the list is the
  same bytes every port ships.

Everything *below* HTTP is deliberately **not** ported. Windows decodes MP3 and AAC
natively and handles HLS, so the desktop builds' HLS worker, MPEG-TS demuxer, ADTS
normalizer and software decoder are unnecessary - they exist there only because
Haiku and Linux have no such support. A side effect: HE-AAC sounds
*better* here than on the desktop builds, which decode only the AAC-LC core layer
because symphonia has no SBR support.

## Module map

```
App.xaml(.cs)                 entry point + startup crash logging
Views/MainPage.xaml(.cs)      the whole UI: country list -> station list -> now playing
Models/Station.cs             station record
Services/StationCatalog.cs    loads the bundled list, lazily per country
Services/StreamResolver.cs    redirect / .m3u / .pls following
Services/RadioPlayer.cs       foreground half of playback: resolve + message the task
Services/AudioMessages.cs     ValueSet keys, duplicated from the component (see below)
Properties/Default.rd.xml     Runtime Directives for the .NET Native compiler
Package.appxmanifest          identity, phone identity, capabilities, splash, bg task

BackgroundAudioComponent/     SEPARATE Windows Runtime Component project
  AudioTask.cs                owns the actual player; survives app suspension
```

The two-project split is not optional - see trap 12.

## The build/deploy traps, in the order they bite

Every item here cost real time. They are recorded so nobody re-derives them.

### 1. `Package.appxmanifest`'s `MinVersion` is not the source of truth

MSBuild **overwrites** it from `<TargetPlatformMinVersion>` in the .csproj. Editing
the manifest alone produces a package that still declares the old minimum. Change
the .csproj, and re-run `/t:Restore` afterwards - NuGet needs to resolve the new
`UAP,Version=vX` framework or the build fails with
`Your project does not reference "UAP,Version=v10.0.10586" framework`.

### 2. A phone silently refuses a package whose minimum is too high

Tapping install does *nothing*: no error, no dialog, no log entry. That is what a
14393-minimum package does on a 10586 phone. If install appears to do nothing,
check the minimum first - it is almost never anything else.

### 3. The compiler does not enforce API availability

With `TargetPlatformMinVersion=10.0.10586.0`, this still compiles clean:

```csharp
var p = new MediaPlayer();
var s = p.PlaybackSession;    // added in 1607 / build 14393
```

Reference assemblies come from the 19041 SDK, so anything in that SDK compiles
regardless of the declared minimum. Verified by building a probe file with each
API in turn: **zero errors for all of them**. The only signal is the device.

This is why nothing here uses `MediaPlayer`/`PlaybackSession`. The first version
did, and touched `PlaybackSession` *in the constructor*, so the app died
immediately after the splash screen on a 10586 phone with nothing to show why.
Playback now goes through `BackgroundMediaPlayer` (trap 12), which exists on
10586.

**When adding any WinRT call, check its "Introduced in" version by hand.** The
build will not.

### 4. `MediaElement` must be in the visual tree

Relevant if you ever put playback back in the foreground: a `MediaElement` plays
nothing if it only exists as a field - it has to be in the tree, though
`Width="0" Height="0"` and `Visibility="Collapsed"` are enough for audio. This app
no longer has one; see trap 12 for why foreground playback was abandoned.

### 5. `AudioCategory.BackgroundCapableMedia` needs a manifest declaration

Setting it without a matching `<Extension Category="windows.backgroundTasks">` makes
the phone terminate the app. Note that having the declaration is *not sufficient*
for background playback either - trap 12.

### 6. `Properties/Default.rd.xml` must be a `Content` item

`Microsoft.NetNative.targets` finds rd.xml by filtering `AppxPackagePayload` for
the `.rd.xml` extension:

```xml
<_IlcInputRdXmlFiles Include="@(AppxPackagePayload)"
  Condition="...EndsWith('.rd.xml'...)"/>
```

`None` and `RdXmlFile` items are both ignored and you get warning ILT0027. That
warning matters: without Runtime Directives the .NET Native optimizer can strip
reflection targets, and the failure appears at runtime on the device.

### 7. Do not pass `--log` to the VS2017 bootstrapper

It auto-updates itself to the modern (4.8) installer engine, which rejects that
option and exits with code **87** having done nothing. The log says
`Option 'log' is unknown`. Logs land in `%TEMP%\dd_*` anyway.

### 8. `Windows10SDK.15063` is no longer installable

The documented SDK for Windows 10 Mobile is gone from the VS installer catalog;
only `Windows10SDK.19041` remains. The `go.microsoft.com` link for the legacy SDK
now redirects to the current one. Compiling against 19041 is fine - see trap 3 for
the catch.

### 9. Signing: use a thumbprint, not a .pfx path

`/p:PackageCertificateKeyFile=x.pfx` fails with `APPX0105: Cannot import the key
file ... may be password protected` even for a password the build knows. Sign from
the certificate store instead:

```powershell
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=RWorldRadio" `
  -KeyUsage DigitalSignature -CertStoreLocation "Cert:\CurrentUser\My" `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
msbuild ... /p:PackageCertificateThumbprint=$($cert.Thumbprint)
```

The certificate subject must equal the manifest's `Publisher` exactly.

### 10. Every deployment tool is broken; MTP copy is what works

| Route | Outcome |
|---|---|
| `WinAppDeployCmd install` | Finds no devices - needs the `IpOverUsb` service |
| Installing `...Windows10SDK.IpOverUsb` | `Cannot find package ... in product graph` - listed in the catalog, payload gone |
| Device Portal over USB (`127.0.0.1:10080`) | Nothing listening - same missing service |
| VS2017 F5 to device | Needs the same USB plumbing |
| **Copy to `Phone\Downloads` over MTP, install from the phone** | **Works** |

Install order on the phone matters: the `.cer` first, then the `.appx`. The app
install fails silently otherwise.

### 11. MTP scripting: session 0 cannot see the phone

The shell's MTP namespace is only visible from an interactive desktop session. An
SSH session lands in session 0 and `Shell.Application` finds no device, with no
error. Run the copy as a scheduled task with `-LogonType Interactive` so it
executes in session 1.

Also: `FolderItem.InvokeVerb('delete')` **hangs** waiting for a confirmation
dialog on the device. Copy under a new filename instead of deleting the old one.

### 12. Background audio needs a separate WinRT component project - three ways to get it wrong

Playback that survives the screen turning off took three failed attempts. Each one
built cleanly and installed; each failed only on the device.

**Attempt 1 - `MediaElement` + manifest declaration.** Added
`<Extension Category="windows.backgroundTasks"><Task Type="audio"/>` and set
`MediaElement.AudioCategory = BackgroundCapableMedia`. Result: audio still stopped
the moment the phone slept. A `MediaElement` belongs to the *foreground view*, so
it is suspended with the app. The manifest declaration does not change that
ownership - it only grants permission the app cannot make use of.

**Attempt 2 - `EntryPoint="RWorldRadio.App"`.** The manifest validator does demand
an `EntryPoint` (`APPX0501` / `error 80080204`), but naming the `App` class just
satisfies the validator. The audio was still owned by the view, so it still stopped.

**Attempt 3 - task class compiled into the app's exe.** Moved the player to a
`BackgroundMediaPlayer` in an `IBackgroundTask`, with the manifest pointing at
`RWorldRadio.BackgroundAudio.AudioTask`. Now playback failed *immediately* with
**`The RPC server is unavailable (0x800706BA)`**.

The reason is worth knowing: the OS activates a background task **by class name over
RPC**, which requires an `ActivatableClass` registration in `AppxManifest.xml`, and
that registration is generated from the **`.winmd` a Windows Runtime Component
produces**. Opening the package showed the problem outright:

```
ActivatableClass entries : NO ActivatableClass entries at all
.winmd in the package    : no .winmd in the package
```

**What works.** The task lives in its own project,
`BackgroundAudioComponent/BackgroundAudioComponent.csproj`, with
`<OutputType>winmdobj</OutputType>`, referenced from the app as a
`<ProjectReference>`. Verify the result by opening the built `.appx` and checking
for both:

```
ActivatableClassId="RWorldRadio.BackgroundAudio.AudioTask"
RWorldRadio.BackgroundAudio.winmd
```

If either is missing, the RPC error is what you will see on the phone.

Two consequences of the split:

- **A WinRT component cannot expose `const` fields across the ABI.** The ValueSet
  message keys are therefore `internal` in `AudioTask.cs` and duplicated in
  `Services/AudioMessages.cs`. They are the wire format between two processes - the
  copies must stay in step.
- **The component project must import the UWP XAML targets**, not
  `Microsoft.CSharp.targets`. The latter makes MSBuild treat it as a .NET Framework
  project and it fails with `MSB3644: The reference assemblies for framework
  ".NETFramework,Version=v4.0" were not found`.

Also required inside the task: hold the `BackgroundTaskDeferral` for the task's
lifetime. Without it the OS tears the task down as soon as `Run()` returns and the
audio dies with it. And answer the `SystemMediaTransportControls` buttons - an app
that declares background audio and ignores them gets its audio stopped.

`BackgroundMediaPlayer` is deprecated in favour of `MediaPlayer` +
`CommandManager`, which needs 14393 and so cannot be used here. The warnings are
suppressed narrowly with `#pragma warning disable CS0618` at each site, with the
reason in a comment.

### 13. Background audio means the UI can outlive nothing - re-sync on resume

Once the audio lives in its own process, the foreground app is the *disposable*
half. The phone suspends and may fully terminate it while playback continues, so
everything the UI held in memory is gone when it comes back. The symptom is a UI
showing **"Stopped" while music is audibly playing**.

Two things are needed, and both are easy to miss:

- **Ask on resume.** `Application.Current.Resuming` sends the task a `query-state`
  message; the task replies with its current state. `OnLoaded` asks too, because a
  cold launch while audio plays is also effectively a resume.
- **The task must send the station name with the state.** The UI cannot supply it -
  its `Station` object is gone. `AudioMessages.NowPlaying` carries it, and
  `RadioPlayer` keeps `_playingName` from whatever the task last reported rather
  than from its own `Current`.

There is a trap inside the fix. A source switch makes the player pass through
Closed/Paused, which the task reports as `stopped`. Treating that as a real stop
makes the label flash "Stopped" on every station change. `RadioPlayer._awaitingStart`
is set when a play request is sent and cleared when the task confirms `playing` (or
reports an error); a `stopped` arriving while it is set is ignored.

## Startup failures are invisible - log them

A sideloaded phone app has no console and no debugger. If it throws during startup
the splash screen simply disappears. `App.xaml.cs` therefore writes to
`LocalState\crash.log`:

- an `UnhandledException` handler,
- a `NavigationFailed` handler,
- `try`/`catch` around `SystemNavigationManager` calls (a plausible second suspect
  for a post-splash crash),
- dataset load failures.

Keep this. It is the only diagnostic channel on the device.

## Deliberate differences from the desktop builds

- **One pane at a time.** No room for two lists on a phone: the country list fills
  the screen, tapping one navigates to its stations, hardware back returns.
- **Single tap to play**, not double-click - there is no separate select gesture.
- **The level meter is not real.** This media stack exposes no metering, so `Level`
  only reflects whether audio is playing. The desktop builds compute a true peak
  from the decoded buffer.
- **Playback is owned by a background process**, unlike the desktop builds where the
  decode loop is in-process. That is what allows it to keep playing with the screen
  off, and it is why the UI talks to it by message-passing rather than method calls.
- **Countries load lazily.** The desktop build parses all ~17 MB at startup; here
  only the index is read up front, and a country's stations on first tap. The
  dataset is ~17 MB expanded and a phone should not chew through it to show a list.

## The `<Content Include>` list

The .csproj lists all 289 dataset files individually, as
`<Content Include="..\shared\data\countries\<slug>.json"><Link>Assets\data\countries\<slug>.json</Link></Content>`,
so they ship in the package under `Assets\data`. If
`../shared/tools/update_stations_db.py` adds or removes country files, that list
must be regenerated or the new files will not be packaged. The list before the
move was confirmed correct: the built package carried exactly 288 country files
plus the index. The `<Link>` form has not been built since - it is standard
MSBuild, but the next Windows build should check the package contents again.

## Build environment that was actually used

Windows 11 ARM64 (build 26200), VS2017 Build Tools 15.9.30, SDK 10.0.19041, under
x86 emulation. VS2017 is a 32-bit x86 program and runs fine there - it is the last
version with phone deployment support, so VS2019+ is not an option even though the
deployment path it provides no longer works (trap 10).

Budget ~17 GB of disk for the Build Tools install.

## Verification status

- `Debug|ARM` and `Release|ARM` (the .NET Native AOT toolchain) both compile with
  no errors and no warnings.
- The signed Release package is 2.97 MB, expanding to ~17.5 MB, and contains
  `countries.json` plus all 288 country files.
  `Get-AuthenticodeSignature` reports Valid; the embedded manifest shows
  `ProcessorArchitecture=arm` and `MinVersion=10.0.10586.0`.
- Installed and running on a **Nokia Lumia 920, Windows 10 Mobile 10586**: starts,
  loads the station list, plays audio, **keeps playing with the screen off**, and
  **still shows the right station after waking**. Switching stations does not flash
  "Stopped". The splash screen is the radio icon on black.

Not verified: any other device or OS build; playback across a broad sample of
stations on the phone (the desktop builds' station sweep has no equivalent here);
`StreamResolver.cs` has no tests - only its Rust original does.
