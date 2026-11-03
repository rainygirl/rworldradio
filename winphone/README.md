# R World Radio for Windows 10 Mobile

English · [한국어](README.ko.md)

<img src="screenshot.png" alt="R World Radio on a Lumia 920" width="320">

Internet radio player for Windows 10 Mobile (Windows Phone 10). Tested on a
Nokia Lumia 920 running Windows 10 Mobile 10586.

## Installing on a phone

A prebuilt, signed package is in [`dist/`](dist), so you can skip the build.

You need a phone on **build 10586 or newer** with developer mode enabled:
**Settings → Update & security → For developers → Developer mode**.

1. Connect the phone to a Windows PC over USB. It appears as `Windows phone`.
2. Copy these into the phone's **Downloads** folder:
   - `dist/RWorldRadio_1.0.0.0_ARM.cer`
   - `dist/RWorldRadio_1.0.0.0_ARM.appx`
   - the three files in `dist/Dependencies/` (only needed if step 5 fails)
3. On the phone, open **File Explorer → Downloads** and tap
   **`RWorldRadio_1.0.0.0_ARM.cer`** first. Confirm the certificate install.
4. Tap **`RWorldRadio_1.0.0.0_ARM.appx`**. It installs and appears in the app list
   as **R World Radio**.
5. If the install complains about a missing framework, tap the three
   `Dependencies` packages first, then repeat step 4. Most phones already have
   them.

Step 3 is not optional - the app install fails without the certificate.

### If tapping install does nothing

That is what a phone does when the package requires a newer OS than it has. Check
the phone's build number under **Settings → About → More info**. This package
requires 10586 or newer.

`WinAppDeployCmd`, Device Portal over USB and Visual Studio's F5 deployment all
need a service Microsoft no longer ships; copying the files to the phone is the
way. See [AGENTS.md](AGENTS.md).

## Building it yourself

You need a **Windows** machine. Windows 11 works, including ARM64.

**1. Install Visual Studio 2017 Build Tools with the UWP workload.** VS2019 and
later dropped Windows Mobile support, so 2017 is the last version that can build
this. Budget about 17 GB of disk.

```
curl -L -o vs_buildtools.exe https://aka.ms/vs/15/release/vs_buildtools.exe
.\vs_buildtools.exe --quiet --norestart --wait ^
    --add Microsoft.VisualStudio.Workload.UniversalBuildTools --includeRecommended
```

Do not add `--log`: the bootstrapper updates itself to a newer installer engine
that rejects that option and quits with code 87 having done nothing.

**2. Create a signing certificate.** Its subject must match the manifest's
`Publisher` exactly. In PowerShell:

```powershell
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=RWorldRadio" `
  -KeyUsage DigitalSignature -FriendlyName "RWorldRadio sideload" `
  -CertStoreLocation "Cert:\CurrentUser\My" `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
$cert.Thumbprint
```

**3. Build and package.** From this directory, with the whole repository checked
out (the station list is read from `..\shared\data`). Sign by *thumbprint*, not
by `.pfx` file - MSBuild's key file import fails with `APPX0105` even when the
password is correct.

```powershell
$msbuild = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2017\BuildTools\MSBuild\15.0\Bin\MSBuild.exe"
& $msbuild RWorldRadio.csproj /t:Restore /p:Configuration=Release /p:Platform=ARM
& $msbuild RWorldRadio.csproj /p:Configuration=Release /p:Platform=ARM `
    /p:PackageCertificateThumbprint=$($cert.Thumbprint)
```

Use `ARM`. Windows 10 Mobile devices are all 32-bit ARM; `x86` only builds for the
phone emulator.

**4. Collect the output** from
`AppPackages\RWorldRadio_1.0.0.0_ARM_Test\`:

```
RWorldRadio_1.0.0.0_ARM.appx     the app
RWorldRadio_1.0.0.0_ARM.cer      install this on the phone first
Dependencies\arm\*.appx          framework packages, if the phone lacks them
```

Then follow "Installing on a phone" above.

If you are changing the code rather than just building it, read
[AGENTS.md](AGENTS.md) first - it documents thirteen traps that each cost real
time, several of which only show up on the device and not at build time.

## Keeping the station list current

The list is fixed at build time. To refresh it, run
`python3 ../shared/tools/update_stations_db.py` and rebuild - see
[AGENTS.md](AGENTS.md) about the `.csproj` file list.

## License

MIT - see [LICENSE](LICENSE).

## AI disclosure

Claude was used in the development of this program.
