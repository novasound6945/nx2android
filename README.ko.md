<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/logo-dark.svg">
    <img alt="nx2android" src="docs/images/logo.svg" height="140">
  </picture>
</h1>

<p align="center">
  <a href="README.md">English</a> &nbsp;·&nbsp; <b>한국어</b>
</p>

<p align="center">
  <a href="https://github.com/shirai91/nx2android"><img alt="shirai91/nx2android의 포크" src="https://img.shields.io/badge/fork%20of-shirai91%2Fnx2android-2f6fed"></a>
  <img alt="기기: Retroid Pocket 6, Nova" src="https://img.shields.io/badge/devices-Retroid%20Pocket%206%20%C2%B7%20Nova-e8590c">
  <img alt="GPU: Adreno 740" src="https://img.shields.io/badge/GPU-Adreno%20740-c92a2a">
  <img alt="드라이버: Turnip" src="https://img.shields.io/badge/driver-Turnip-7048e8">
  <img alt="라이선스: MIT" src="https://img.shields.io/badge/license-MIT-2b8a3e">
</p>

> [!IMPORTANT]
> **개인 포크이며 원본 프로젝트가 아닙니다.** nx2android는 [shirai91](https://github.com/shirai91)과 기여자들의 작업이고, seedds의 [nxnative](https://github.com/seedds/nxnative) 위에 만들어졌습니다. 로더, 호스트, NVN→Vulkan 리플레이, porter, 그리고 여기 있는 게임 프로파일 거의 전부가 그분들의 것입니다. 이 포크의 모든 것은 그 작업 위에 서 있으며 감사드립니다. 릴리스·전체 게임 목록·지원은 **[shirai91/nx2android](https://github.com/shirai91/nx2android)** 로 가세요.

| | |
|---|---|
| 🎯 | **대상 기기.** **Retroid Pocket 6**와 **Retroid Pocket Nova**(Snapdragon 8 Gen 2, **Adreno 740**) + **Turnip** Vulkan 드라이버뿐입니다. 모든 기본값, 프로파일 설정, 측정값이 이 GPU 기준이고 다른 기기에서는 아무것도 시험하지 않습니다. 다른 기기에서는 될 수도, 업스트림보다 나쁠 수도, 아예 안 될 수도 있으니 업스트림을 쓰세요. |
| 🧩 | **이 포크는** 그 기기들에서 게임 몇 개를 더 띄우고 필요했던 수정을 얹은 것입니다. 업스트림 `main`과 계속 병합하며, 작업은 `ports` 브랜치에 있고 같은 MIT 라이선스를 따릅니다. 개발은 Windows PC에서 합니다. |
| 🛠️ | **누구를 위한 것.** 받아서 자기에게 맞게 고쳐 쓰려는 분들을 위한 것입니다. 있는 그대로 제공하며 릴리스도 지원도 없습니다. 체크아웃해서 실행하세요([docs/DEVELOPMENT_GUIDE.md](docs/DEVELOPMENT_GUIDE.md)). |
| ⚖️ | **책임.** 게임도, 키도, 거기서 추출한 것도 들어 있지 않고 그런 곳으로 연결하지도 않습니다. 자기 콘솔에서 게임과 키를 덤프하는 일, 포팅하는 게임을 소유하는 일, 사는 곳의 법이 허용하는 범위는 전적으로 각자의 책임입니다. 만든 APK와 데이터 팩은 공유하지 마세요. |
| 🐞 | **문제가 생기면.** 이 포크의 변경이 원인이라고 보고, 업스트림이 아니라 여기에 알려 주세요. |

<p align="center">
  <b>이 포크가 더한 것</b><br>
  <a href="#이-포크에서-띄운-게임">게임</a> &nbsp;·&nbsp;
  <a href="#첫-실행-때-셰이더-준비">셰이더 준비</a> &nbsp;·&nbsp;
  <a href="#게임-데이터를-내-서버에서-받기">내 서버에서 데이터 받기</a> &nbsp;·&nbsp;
  <a href="#그-밖에-이-포크에서-바뀐-것">그 밖의 변경</a> &nbsp;·&nbsp;
  <a href="docs/PORTING_NOTES.md">포팅 노트(영어)</a>
</p>

---

자기가 가진 닌텐도 스위치 게임을 안드로이드로 네이티브 포팅하는 도구입니다. 게임의 arm64 코드가 기기에서 그대로 실행되고, 스위치의 시스템 서비스는 호스트가 대신하며, NVN 그래픽은 Vulkan으로 다시 그려집니다. 게임 파일과 키는 직접 준비해야 하고, 이 저장소에는 어떤 게임의 것도 들어 있지 않습니다.

이 문서는 **이 포크에서 달라진 부분**만 한국어로 옮긴 것입니다. 도구 설치와 사용법, 세이브, 원본이 지원하는 게임 전체 목록은 [영어 README](README.md)와 [packaging/INSTALL.md](packaging/INSTALL.md)를 보세요.

## 이 포크에서 띄운 게임

Retroid Pocket 6와 Retroid Pocket Nova(둘 다 Snapdragon 8 Gen 2, Adreno 740, Android 13)에서 Turnip 드라이버 "PurpleVK T30"(`--vulkan-driver`)으로 시험했습니다. 아시아판은 타이틀 ID가 달라서 프로파일도 따로 있습니다.

| 게임 | 버전 | 기기 | 결과 |
|---|---|---|---|
| 젤다의 전설 꿈꾸는 섬 | 업데이트 1.1.0 | Retroid Pocket 6 | **플레이 가능**, 60 fps 유지. 피사계심도(틸트시프트) 흐림은 기본으로 꺼 둠(프로파일의 한 줄로 다시 켤 수 있음). 독·휴대 모드 모두 되고 실행 중 전환도 됨. 한글은 `--shared-font`로 |
| 마계전기 디스가이아 5 | v196608 (SDK 1.7.1) | Retroid Pocket Nova | **플레이 가능**, 60 fps: 로고, 오프닝 동영상, 타이틀, 필드, 전투. 효과음·음악·음성 모두 나옴. 이렇게 오래된 SDK는 게임 프로세스 안에 오디오 렌더러가 없어서, 호스트가 `audren:u`를 직접 서비스하고 소리를 섞음 |
| 마계전기 디스가이아 4 리턴 | 본편 | Retroid Pocket 6 | 타이틀, 새 게임, 오프닝, 거점까지 60 fps. 전투는 아직 확인 안 함 |
| 13기병방위권 (아시아판) | 본편 | Retroid Pocket 6 | **플레이 가능**, 60 fps. 가장 무거운 로봇 컷신(프레임당 드로 약 6,000개)에서는 떨어짐 |
| 유니콘 오버로드 (아시아판) | 업데이트 1.05 | Retroid Pocket 6 | **플레이 가능**, 대부분의 장면에서 60 fps. 가장 큰 맵(프레임당 드로 약 6,800개)은 GPU 한계 |
| 다크 소울 리마스터 | 업데이트 1.0.3 | Retroid Pocket Nova | 게임 속도는 그대로인 채 60 fps 유지(스위치는 30). 4:3 화면 지원. 새 파이프라인에서 색이 번쩍이는 문제 없음. `env.txt`에 `DM_FPS_TARGET=30`을 넣으면 원래의 30 fps |
| 파이어 엠블렘 인게이지 | 업데이트 2.0.0 | Retroid Pocket Nova | **플레이 가능**, 60 fps(스위치는 30): 1·2장, 맵, 전투와 전투 장면, 한국어. 파일 206,899개를 묶음 89개로 전송 |
| 테일즈 오브 이터니아 리마스터 | 업데이트 2.1.0 | Retroid Pocket 6 | **플레이 가능**, 60 fps 유지 |
| 마인크래프트 던전스 | 본편 | Retroid Pocket 6 | **작업 중**: 부팅되고 첫 레벨을 소리와 함께 30 fps로 플레이. 런타임과 도구 쪽 지원(오디오 렌더러 두 개 동시, Unreal pak 버전 8)은 들어 있고, 프로파일은 아직 커밋하지 않음 |

아직 안 되는 것: 마인크래프트 던전스 II는 로고 직후에 멈춥니다([docs/PORTING_NOTES.md](docs/PORTING_NOTES.md#3-open-problem-minecraft-dungeons-ii-stops-after-the-logo)).

## 첫 실행 때 셰이더 준비

게임이 처음 만나는 파이프라인은 원래 플레이 도중에 드라이버가 통째로 컴파일해야 했습니다. 그래서 새 효과가 나올 때마다 끊기거나, 파이프라인이 준비될 때까지 그 물체가 그려지지 않았습니다. `VK_EXT_graphics_pipeline_library`를 지원하는 드라이버(Turnip은 지원, Retroid Pocket 6의 퀄컴 기본 드라이버는 미지원)에서는, 앱이 게임 시작 전에 진행 화면을 띄우고 모든 셰이더의 부품을 한 번 만들어 둡니다. 다크 소울 리마스터의 셰이더 3,099개가 Retroid Pocket 6에서 약 17초 걸립니다. 플레이 중에는 새 파이프라인을 그 부품들로 약 0.03 ms 만에 이어 붙이므로 빠지는 것이 없고, 완전히 최적화된 파이프라인은 뒤에서 만들어져 교체됩니다.

- 팩과 드라이버 조합마다 한 번만 합니다. 다음 실행부터는 표시를 찾아 약 0.5초 만에 확인만 합니다.
- 이 기능을 쓰려면 Turnip 드라이버로 포팅하세요(`--vulkan-driver <파일>`). 확장이 없는 드라이버에서는 예전 방식으로 동작합니다.
- `env.txt`에 `DM_VK_GPL=0`을 넣으면 끕니다. 첫 실행 상태로 되돌리려면 게임을 끈 채로 `files/vk_gpl.id`, `vk_pipeline_cache.bin`, `vk_pipelines.bin`을 지우세요.

구조 설명: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)의 "Graphics pipeline libraries".

## 게임 데이터를 내 서버에서 받기

포트는 APK와 수 GB짜리 데이터 팩으로 이루어지고, 보통 USB로 복사합니다. `--data-url`로 포팅하면 APK만 설치해도 됩니다. PC에 한 번도 연결한 적 없는 기기에서도 첫 실행 때 내 웹 서버에서 팩을 받아옵니다. 그 뒤로는 실행할 때마다 서버의 목록과 기기의 목록을 비교해 바뀐 것만 받으므로, 팩을 새로 만들어도 케이블 없이 기기에 반영됩니다.

1. 서버 폴더 주소를 주고 포팅합니다: `porter port <게임 폴더> --data-url https://<호스트>/<경로>/` (서버가 Basic 인증을 요구하면 `user:password` 한 줄이 든 파일을 `--data-cred <파일>`로 추가).
2. `<App>-data` 폴더를 그 주소에 그대로 올립니다. `files.json`은 맨 마지막에 올리세요. Range가 되는 GET에 응답하는 HTTP·HTTPS 서버면 됩니다(예: NAS의 WebDAV 공유). 디렉터리 목록 기능은 필요 없습니다.
3. APK를 기기에 설치하고 실행합니다.

팩을 고친 뒤에는 `porter publish <App>-data`가 목록을 다시 씁니다(바뀌지 않은 파일은 해시를 유지하므로 4 GB짜리 RomFS는 한 번만 해시합니다). 작은 파일이 아주 많은 팩은 묶음으로 보낼 수 있습니다(프로파일 `android.tsv`의 `data_bundles`). 파이어 엠블렘 인게이지의 작은 파일 205,000개가 묶음 89개가 됐습니다.

주소와 `user:password`는 APK 안에 들어가며, 가려져 있을 뿐 암호화되지는 않습니다. APK를 가진 사람은 누구나 읽을 수 있습니다. 이 용도로 만든 읽기 전용 계정을 쓰고, 서버는 비공개로 두고, APK를 공유하지 마세요. 팩은 자기 덤프이고, 이 기능은 자기 기기를 위한 것입니다.

구조 설명: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)의 "The data from a web server".

## 그 밖에 이 포크에서 바뀐 것

- **소리.** 오디오 렌더러 여러 개 동시 지원(마인크래프트 던전스는 동영상용과 게임용을 따로 엽니다). SDK 1.x 게임(디스가이아 5)을 위해 호스트가 실제 믹서와 함께 `audren:u`를 서비스.
- **동영상.** 프레임을 Vulkan 이미지에 바로 올리고 색 변환에 NEON을 씁니다. 1080p60 VP9 오프닝에서 내용이 실제로 바뀌는 화면 프레임이 72~80%에서 95~96%로 올랐습니다.
- **그래픽.** 파이프라인 라이브러리 사용 시 버텍스 스테이지의 유니폼 버퍼만 동적으로(Turnip에서 다크 소울 리마스터의 색 번쩍임, 파이어 엠블렘 인게이지의 기기 손실 해결). NVN 알파 테스트를 프래그먼트 셰이더 변형으로 구현. 조건부 렌더링에 실제 오클루전 쿼리 사용. present에서 레이어 크롭 반영. 알파 테스트 discard를 제거할 셰이더의 게임별 목록(`DM_VK_FS_NOKILL`). 한 텍스처 바인딩에 다른 바인딩의 텍스처를 주기(`DM_VK_BIND_ALIAS`, 꿈꾸는 섬의 흐림 끄기).
- **실행.** 게임 실행 중 독·휴대 모드 전환(`DM_CTL` `opmode`)과 게임이 기다리는 컨트롤러 이벤트. `DM_GPU_TURBO`는 앱이 뒤로 가면 GPU 클럭 제어를 돌려줍니다. 일시정지 메뉴의 프레임 제한.
- **도구.** Unreal pak 버전 8과 버전 1 셰이더 아카이브(UE 4.22). 디스가이아의 셰이더 컨테이너. IoStore 셰이더 라이브러리가 있는 pak 폴더를 인라인 셰이더로 오인하지 않음.
- **진단.** 멈춘 게임의 스레드 덤프, 렌더러별 오디오 덤프, 몇 초에 걸친 패스별 통계, 1초만 나타나는 효과를 잡는 유니폼 버퍼 로그와 트랩.

## 키와 게임 파일

nx2android는 포팅하는 동안 사용자의 `prod.keys`로 게임 파일을 복호화합니다. 스위치 에뮬레이터와 같은 방식입니다. 이 저장소에는 키도, 게임 파일도, 거기서 추출한 것도 없고, 도구가 그런 것을 내려받지도 않습니다. 둘 다 자기 스위치에서 직접 덤프하고, 그것들과 그것으로 만든 APK·데이터 팩을 공유하지 마세요. 자기 게임을 자기가 쓰려고 복호화하는 것이 허용되는지는 사는 곳에 따라 다릅니다.

## 라이선스

MIT입니다(nxnative와 같음, `nxnative/LICENSE` 참고). 서드파티 코드는 각자의 라이선스를 따릅니다.
