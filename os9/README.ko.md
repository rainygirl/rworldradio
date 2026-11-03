# R World Radio for Mac OS 9

[English](README.md) · 한국어

Mac OS 9(PowerPC)용 인터넷 라디오입니다. 두 부분으로 되어 있습니다. Mac의
클래식 Toolbox 앱과, 같은 네트워크의 Linux 머신에서 방송국을 받아 변환해 주는
작은 릴레이입니다 (Mac OS 9에는 요즘 TLS, AAC, HLS가 없습니다).

## 1. 릴레이 (Linux, 예: Raspberry Pi)

Python 3과 libmp3lame이 들어간 ffmpeg가 필요합니다. 이 디렉터리에서:

```sh
python3 relay/radio_relay.py --port 8083        # ../shared/data를 읽습니다
```

서비스로 계속 띄워 두려면:

```sh
sudo cp relay/radio-relay.service /etc/systemd/system/
sudo systemctl enable --now radio-relay
```

서비스 파일은 체크아웃이 `/home/rainygirl/Workspace/robot/tiger/radio`에 있다고
가정합니다. `WorkingDirectory`와 `ExecStart`를 실제 위치로 고치세요.

## 2. Mac 앱

요구 사항: QuickTime 6, URL Access, Open Transport가 있는 Mac OS 9. 메모리 파티션
24 MB (최소 12 MB).

Docker가 있는 아무 머신에서 빌드합니다 (Retro68이 컨테이너에서 돕니다).

```sh
make            # -> build/mac/RWorldRadio.bin (MacBinary)
```

`RWorldRadio.bin`을 Mac으로 복사해 StuffIt Expander(또는 리소스 포크를 보존하는
MacBinary 도구)로 풉니다.

앱 옆에 릴레이 주소를 적은 텍스트 파일 `R WorldRadio Settings`(CR 줄 끝)를
만듭니다.

```
relay=10.0.0.129:8083
last=kexp            (또는 방송국 id. 비우면 시작 때 아무것도 재생하지 않음)
lastname=KEXP 90.3 FM / Seattle
```

이후에는 앱이 이 파일을 직접 씁니다.

## 라이선스

MIT. [LICENSE](LICENSE)를 참고하세요.

## AI 사용 고지

이 프로그램 개발에는 Claude가 활용되었습니다.
