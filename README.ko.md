# R World Radio - Mac OS X 10.4 PowerPC 포트 (C / Carbon)

[English README](README.md)

PowerPC 맥의 Mac OS X Tiger에서 도는 인터넷 라디오 플레이어. 

![Mac OS X 10.4에서 실행 중인 R World Radio](screenshot.png)

## 요구 사항

| | |
|---|---|
| 머신 | iMac G4, 또는 PowerPC 745x G4 아무거나 |
| 시스템 | Mac OS X 10.4 |
| RAM | 256MB (앱 상주 메모리는 약 25MB) |
| 툴체인 | Xcode 2.5 - GCC 4.0.1과 10.4u SDK |
| 그 외 | 네트워크 |

빌드가 `-mcpu=7450`이라 G3에서는 돌지 않고, Intel 슬라이스도 없다.

## 빌드에는 머신 두 대가 필요하다

Tiger는 자기 빌드 입력을 스스로 준비하지 못한다. `curl`이 요즘 TLS 호스트에 못 붙어
아무것도 내려받지 못하고, Python 2.3까지만 있어서 여기 생성 스크립트가 하나도 돌지
않는다. 그래서 생성물은 전부 최신 맥에서 만들어 옮기고, PowerPC 머신에서는 컴파일만
한다.

저장소를 clone하는 것만으로도 부족하다. 빌드 입력 세 개가 저장소에 없고 생성해야
한다.

| | 출처 |
|---|---|
| `vendor/` 소스 | `tools/fetch-vendor.sh`가 mbedTLS, libmad, faad2를 받는다 |
| `data/cacert.der` | `make castore` - 빌드 머신의 PEM CA 번들에서 |
| `data/stations.dat` | `make dataset` - 이미 `data/`에 있는 방송국 JSON에서 |

### 1. 최신 맥에서

```sh
tools/fetch-vendor.sh    # mbedTLS, libmad, faad2를 vendor/ 에 받는다
make castore             # data/cacert.der - 호스트의 CA 번들에서
make dataset             # data/countries*.json -> data/stations.dat
make icons               # packaging/rworldradio.icns 와 packaging/icon_png.h
```

방송국 JSON은 저장소에 들어 있으므로 `make dataset`에 인자가 필요 없다. 다른 포트의
`data/` 디렉터리에서 만들려면 `DATASET_SRC=<디렉터리>`를 넘기면 된다.

그다음 체크아웃을 통째로 Tiger 머신에 옮긴다. `data/`, `packaging/*.icns`,
`packaging/icon_png.h`, `vendor/`의 tarball을 모두 포함해서.

### 2. Tiger 머신에서

```sh
tools/fetch-vendor.sh    # 이미 있는 tarball을 풀기만 한다 - 네트워크 불필요
make                     # -> build/rworldradio
make bundle              # -> build/RWorldRadio.app
```

`tools/fetch-vendor.sh`는 이미 있는 tarball을 건너뛰므로, 여기서는 압축을 풀고 벤더
`config.h` 두 개를 쓰는 일만 한다.

## 설치

```sh
./install.sh             # /Applications 로
./install.sh --user      # ~/Applications 로
./install.sh --prefix DIR
```

번들은 자기완결적이다. 방송국 데이터셋과 CA 스토어가 `Contents/Resources` 안에 들어
있으므로 지워도 아무것도 남지 않는다.

```sh
./install.sh --uninstall
```

또는 그냥 `RWorldRadio.app`을 휴지통에 끌어 넣으면 된다.

`build/rworldradio`는 번들 없이 빌드 트리에서 바로 실행해도 되고, 옆의 `data/`를
찾아 쓴다.

## G4 없이 빌드 확인하기

```sh
make check               # 체크 162개, 호스트 컴파일러
make check-sanitize      # 같은 걸 ASan, UBSan, TSan 아래에서
```

포트의 프로토콜/데이터 절반을 **호스트** 컴파일러로 빌드해, 실제로 빌드에 들어가는
`data/stations.dat`과 `data/cacert.der`를 대상으로 테스트를 돌린다. Carbon,
CoreAudio, 디코더 둘은 포함되지 않는다.

같은 절반을 진짜 빅엔디안 PowerPC에서, 7450을 단 에뮬레이트된 Power Mac 위에서
돌리려면:

```sh
brew install qemu llvm lld
tools/ppc-verify/verify.sh
```

자세한 건 [`tools/ppc-verify/README.md`](tools/ppc-verify/README.md) 참고.

## 알려진 한계

- **Ogg, Opus, FLAC, MP4/M4A, WAV는 재생되지 않는다.** 51,443개 중 505개, 1% 미만.
  조용히 실패하는 대신 상태줄이 포맷 이름을 말해준다.
- **TLS 1.3 미지원.** 이를 요구하는 소수 HTTPS 호스트는 핸드셰이크 실패로 뜬다.
- **미디어 키와 재생 중 메타데이터 없음.**

## 라이선스

여기 소스는 **MIT**다 - [`LICENSE`](LICENSE) 참고. `src/`, `tests/`, `tools/`,
`packaging/`을 포함하며 그 조건으로 따로 재사용할 수 있다.

**빌드된 바이너리는 GPL-2.0-or-later**다. libmad와 faad2(둘 다 GPL-2.0-or-later),
mbedTLS(Apache-2.0 / GPL-2.0-or-later 듀얼 라이선스 - 여기서는 GPL 쪽을 택함)를 정적
링크하기 때문이다. 바이너리를 배포하면 GPL에 따라 저 세 라이브러리의 대응 소스도 함께
제공해야 한다. faad2의 AAC 특허 조항과 함께 [`vendor/README.md`](vendor/README.md)에
정리해 뒀다.

