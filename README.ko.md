# <img src="renkuos/icon.png" width="32" height="32" alt=""> R World Radio

인터넷 라디오 플레이어입니다. 국가를 고르고 방송국을 고르면 재생됩니다 - 229개국
약 51,000개 방송국 목록이 앱에 함께 들어 있습니다. 듣기로 고른 방송국 외에는
네트워크에 접근하지 않습니다.

[English](README.md) · 한국어

저장소 하나에 포트 여섯 개입니다. 포트마다 디렉터리가 하나씩 있고 설치 안내도 각
디렉터리에 있습니다. 방송국 목록과 Rust 포트들이 공유하는 코드는
[`shared/`](shared)에 있습니다.

| 포트 | 플랫폼 | 설치 안내 |
|---|---|---|
| [`renkuos/`](renkuos) | RenkuOS (HaikuOS): x86_gcc2, x86_64, arm64 | [renkuos/README.ko.md](renkuos/README.ko.md) - `pkgman install rworldradio` |
| [`linux/`](linux) | Linux 데스크톱 (X11 / XWayland) | [linux/README.ko.md](linux/README.ko.md) |
| [`mac/`](mac) | macOS 10.15 이상, Apple silicon과 Intel | [mac/README.ko.md](mac/README.ko.md) |
| [`macppc/`](macppc) | PowerPC G4의 Mac OS X 10.4 Tiger | [macppc/README.ko.md](macppc/README.ko.md) - 미리 빌드한 dmg는 `macppc/dist/` |
| [`os9/`](os9) | PowerPC의 Mac OS 9, Linux 머신의 릴레이와 함께 | [os9/README.ko.md](os9/README.ko.md) - 미리 빌드한 앱은 `os9/dist/` |
| [`winphone/`](winphone) | Windows 10 Mobile (Windows Phone 10) | [winphone/README.ko.md](winphone/README.ko.md) |

## 스크린샷

### RenkuOS (HaikuOS)

![RenkuOS에서 실행한 R World Radio](renkuos/screenshot.png)

### Linux

![Linux에서 실행한 R World Radio](linux/screenshot.png)

### macOS

![macOS에서 실행한 R World Radio](mac/screenshot.png)

### Mac OS X 10.4 (PowerPC)

![Mac OS X 10.4에서 실행한 R World Radio](macppc/screenshot.png)

### Mac OS 9

![Mac OS 9에서 실행한 R World Radio](os9/screenshot.png)

### Windows 10 Mobile

<img src="winphone/screenshot.png" alt="Lumia 920에서 실행한 R World Radio" width="320">

## 구성

```
shared/data/     방송국 목록: countries.json + countries/<slug>.json (radio-browser)
shared/core/     Linux와 macOS 포트가 쓰는 Rust 크레이트
shared/tools/    update_stations_db.py - radio-browser에서 shared/data를 갱신
renkuos/ linux/ mac/ macppc/ os9/ winphone/   포트별 디렉터리
```

포트들이 서로 어떤 관계인지, 어떻게 고치는지 등 설치 이외의 내용은
[AGENTS.md](AGENTS.md)와 각 포트 디렉터리의 `AGENTS.md`에 있습니다.

## 방송국 목록 출처

- [radio-browser](https://www.radio-browser.info/)

## 라이선스

MIT. [LICENSE](LICENSE)를 참고하세요. PowerPC와 arm64 빌드는 별도 조건의
서드파티 디코더를 링크합니다. [macppc/README.ko.md](macppc/README.ko.md#라이선스)와
[renkuos/THIRD-PARTY-NOTICES.md](renkuos/THIRD-PARTY-NOTICES.md)를 참고하세요.

## AI 사용 고지

이 프로그램 개발에는 Claude가 활용되었습니다.
