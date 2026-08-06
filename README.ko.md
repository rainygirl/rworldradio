# R World Radio - Windows 10 Mobile

*[English version](README.md)*

Windows 10 Mobile(Windows Phone 10)용 인터넷 라디오 플레이어입니다. 국가를 고르고
방송국을 고르면 재생됩니다 - 229개국 약 51,000개 방송국 목록이 앱에 함께 들어있습니다.

Nokia Lumia 920, Windows 10 Mobile 10586에서 테스트했습니다.

<img src="screenshot.png" alt="Lumia 920에서 실행 중인 R World Radio" width="320">

## 폰에 설치하기

서명된 패키지가 [`dist/`](dist)에 들어있어서 빌드를 건너뛸 수 있습니다.

**빌드 10586 이상**이고 개발자 모드가 켜진 폰이 필요합니다:
**설정 → 업데이트 및 보안 → 개발자용 → 개발자 모드**.

1. 폰을 Windows PC에 USB로 연결합니다. `Windows phone`으로 나타납니다.
2. 다음 파일들을 폰의 **Downloads** 폴더로 복사합니다:
   - `dist/RWorldRadio_1.0.0.0_ARM.cer`
   - `dist/RWorldRadio_1.0.0.0_ARM.appx`
   - `dist/Dependencies/`의 파일 3개 (5번에서 실패할 때만 필요)
3. 폰에서 **파일 탐색기 → Downloads**를 열고 **`RWorldRadio_1.0.0.0_ARM.cer`를
   먼저** 탭해 인증서를 설치합니다.
4. **`RWorldRadio_1.0.0.0_ARM.appx`** 를 탭하면 설치되고, 앱 목록에
   **R World Radio** 로 나타납니다.
5. 프레임워크가 없다는 오류가 나오면 `Dependencies` 패키지 3개를 먼저 탭한 뒤 4번을
   다시 하세요. 대부분의 폰에는 이미 들어있습니다.

### 설치를 눌러도 아무 일도 일어나지 않으면

패키지가 요구하는 OS 버전이 폰보다 높을 때 폰이 그렇게 반응합니다(오류 메시지도
나오지 않습니다). **설정 → 정보 → 자세한 정보**에서 빌드 번호를 확인하세요. 이 패키지는
10586 이상이 필요합니다.

### 동작하지 않는 도구들

공식 문서에 나오는 방법을 시도하실 경우를 위해 적어둡니다. `WinAppDeployCmd`, USB를 통한
Device Portal, Visual Studio의 F5 배포는 모두 `IpOverUsb` 서비스가 필요한데 Microsoft가
더 이상 배포하지 않습니다. 파일을 폰으로 복사하는 방법이 유일하게 동작합니다. (Wi-Fi를
통한 Device Portal은 폰에서 켜면 여전히 동작할 것입니다.)

## 직접 빌드하기

**Windows** 머신이 필요합니다. Windows 11도 되고, ARM64도 됩니다.

**1. Visual Studio 2017 Build Tools와 UWP 워크로드를 설치합니다.** VS2019 이후는
Windows Mobile 지원이 빠져서, 2017이 이걸 빌드할 수 있는 마지막 버전입니다. 디스크는
17GB 정도 필요합니다.

```
curl -L -o vs_buildtools.exe https://aka.ms/vs/15/release/vs_buildtools.exe
.\vs_buildtools.exe --quiet --norestart --wait ^
    --add Microsoft.VisualStudio.Workload.UniversalBuildTools --includeRecommended
```

`--log`은 넣지 마세요. 부트스트래퍼가 스스로를 최신 설치 엔진으로 업데이트하는데, 그
엔진이 이 옵션을 거부하고 아무 일도 하지 않은 채 코드 87로 종료합니다.

**2. 서명 인증서를 만듭니다.** 주체(subject)가 매니페스트의 `Publisher`와 정확히 같아야
합니다. PowerShell에서:

```powershell
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=RWorldRadio" `
  -KeyUsage DigitalSignature -FriendlyName "RWorldRadio sideload" `
  -CertStoreLocation "Cert:\CurrentUser\My" `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
$cert.Thumbprint
```

**3. 빌드하고 패키징합니다.** `.pfx` 파일이 아니라 **지문(thumbprint)** 으로 서명하세요.
MSBuild의 키 파일 가져오기는 비밀번호가 맞아도 `APPX0105`로 실패합니다.

```powershell
$msbuild = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2017\BuildTools\MSBuild\15.0\Bin\MSBuild.exe"
& $msbuild RWorldRadio.csproj /t:Restore /p:Configuration=Release /p:Platform=ARM
& $msbuild RWorldRadio.csproj /p:Configuration=Release /p:Platform=ARM `
    /p:PackageCertificateThumbprint=$($cert.Thumbprint)
```

`ARM`을 쓰세요. Windows 10 Mobile 기기는 모두 32비트 ARM이고, `x86`은 폰 에뮬레이터용
빌드입니다.

**4. 결과물은** `AppPackages\RWorldRadio_1.0.0.0_ARM_Test\` 에 있습니다:

```
RWorldRadio_1.0.0.0_ARM.appx     앱
RWorldRadio_1.0.0.0_ARM.cer      폰에 먼저 설치할 인증서
Dependencies\arm\*.appx          프레임워크 패키지 (폰에 없을 때만 필요)
```

그다음 위의 "폰에 설치하기"를 따르세요.

빌드만 하는 게 아니라 코드를 수정하실 거라면 [AGENTS.md](AGENTS.md)를 먼저 읽어보세요.
실제로 시간을 잡아먹은 함정 13가지가 정리되어 있고, 그중 여러 개는 빌드 시점이 아니라
실기에서만 드러납니다.

## 방송국 목록 최신 상태로 유지하기

목록은 빌드 시점에 고정됩니다. 갱신하려면 다시 빌드해야 합니다 -
[AGENTS.md](AGENTS.md)와 Linux 저장소의 데이터셋 도구를 참고하세요.

## 지원하지 않는 것

- **Opus 스트림은 재생되지 않습니다.** MP3, AAC, HLS, Ogg/Vorbis는 됩니다.
- **암호화된 HLS 스트림(`#EXT-X-KEY`)은 재생되지 않습니다.**
- 일부 방송국은 이미 죽었거나, 브라우저처럼 보이지 않는 접속을 거부하거나, 자국 내에서만
  접속을 허용합니다.

## 방송국 목록 출처

- [radio-browser](https://www.radio-browser.info/)

## 라이선스

MIT - [LICENSE](LICENSE) 참고.

## AI 기여 고지

이 프로그램은 Claude와 함께 작업해 만들었습니다.
