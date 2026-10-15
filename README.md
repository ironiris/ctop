# ctop

NVIDIA GPU, 시스템 메모리, 프로세스, 네트워크 및 디스크 I/O를 터미널에서 확인하는 가벼운 모니터링 도구입니다.

C11 기반 단일 C 프로그램이며 curses 라이브러리를 사용하지 않습니다. GPU 정보는 NVML을 통해 가져오고, 시스템 정보는 Linux의 `/proc` 인터페이스에서 읽습니다.

[English README](README.en.md)

![ctop 터미널 화면](docs/images/ctop.png)

![ctop 웹 대시보드](docs/images/ctop_web.png)

## 주요 기능

- GPU 온도, 전력, VRAM, GPU 사용률 표시
- GPU를 사용하는 프로세스 목록과 GPU Load 표시
- RAM/SWAP 사용량 및 히스토리 그래프
- 디스크 I/O 실시간 그래프와 최근 400초 최대 피크 기준 Y축
- VRAM/GPU 사용률 히스토리 그래프
- 네트워크 처리량을 bit/s 단위로 표시
- 인터페이스별 실제 링크 속도를 기준으로 네트워크 사용률 계산
- RX/TX 방향 표시 및 인터페이스별 네트워크 정보
- Docker 관련 인터페이스(`docker*`, `br-*`, `veth*`) 제외
- 마우스로 섹션 접기/펼치기
- 선택 가능한 alternate-screen 모드

## 필요한 환경

### 컴파일 시

- Linux
- C11을 지원하는 GCC
- CUDA/NVML 개발 파일
  - `nvml.h`
  - 일반적으로 CUDA의 `targets/x86_64-linux/lib/stubs` 아래에 있는 NVML stub 라이브러리
- 기본 glibc, `libdl`, `libm`

빌드 스크립트는 `CUDA_ROOT`(지정한 경우), `/usr/local/cuda*`, `/opt/cuda*`, `/usr` 순으로 NVML 개발 파일을 찾습니다. 찾지 못으면 빌드를 중단하고 `CUDA_ROOT` 지정 방법을 알려줍니다.

의존성만 확인하려면:

```bash
./build-ctop.sh --check
```

### 실행 시

- 지원되는 NVIDIA GPU
- NVML을 제공하는 NVIDIA 드라이버
- `nvidia-smi`가 정상적으로 실행되는 환경
- TUI 실행 시 Linux의 실제 터미널(TTY)과 최소 터미널 크기 76열 x 13행 필요
- 웹 전용(`--no-tui`) 실행에는 TTY 불필요

다음 `/proc` 및 `/sys` 파일을 사용합니다.

```text
/proc/meminfo
/proc/net/dev
/proc/stat
/proc/uptime
/proc/diskstats
/sys/class/net/<interface>/speed
```

## 사전 설치

Ubuntu/Debian 계열에서 기본 빌드 도구를 설치합니다.

```bash
sudo apt update
sudo apt install build-essential
```

NVIDIA GPU에 맞는 드라이버를 설치한 뒤 확인합니다.

```bash
nvidia-smi
```

이후 NVIDIA 공식 CUDA 설치 안내에 따라 CUDA Toolkit을 설치합니다. CUDA Toolkit에는 `nvml.h`와 NVML stub 라이브러리가 포함되어 있어야 합니다.

CUDA 경로가 `/usr/local/cuda-12.8`이 아닌 경우 설치 경로를 확인합니다.

```bash
find /usr/local -name nvml.h 2>/dev/null
```

## 컴파일

프로젝트 디렉터리에서 실행합니다.

```bash
./build-ctop.sh
```

CUDA가 다른 경로에 설치되어 있다면:

```bash
CUDA_ROOT=/usr/local/cuda ./build-ctop.sh
```

정상적으로 완료되면 프로젝트 디렉터리에 `ctop` 실행 파일이 생성됩니다. 웹 대시보드의 HTML/CSS/JS 리소스도 이때 실행 파일에 포함됩니다.

## 설치

컴파일한 실행 파일을 `/usr/local/bin/ctop`에 설치합니다.

```bash
sudo ./install-ctop.sh
```

## 실행

```bash
./ctop
```

주요 옵션:

```text
-i, --interval N          갱신 주기(초)
-p, --port PORT           웹 대시보드를 지정한 포트로 함께 실행
--no-tui                  TUI 없이 웹 대시보드만 실행(-p 필요)
--localhost               웹 접속을 127.0.0.1로 제한(-p 필요)
-t, --token TOKEN         웹 대시보드와 API에 접근 토큰 요구(-p 필요, CTOP_TOKEN 환경변수도 가능)
--no-color                ANSI 색상 끄기
--debug-log FILE          디버그 로그 파일
-a, --alternate-screen    종료 시 기존 화면 복원
-r, --restore-screen      --alternate-screen 별칭
-v, --version             버전 표시
```

예시:

```bash
./ctop -i 1
./ctop --no-color
./ctop --alternate-screen
./ctop -p 8079
./ctop -p 8079 --localhost
./ctop -p 8079 --token "$(openssl rand -hex 16)" --localhost
./ctop --no-tui --port 8079
```

## 웹 대시보드와 API

`-p 8079`를 지정하면 기존 터미널 화면과 웹 대시보드를 함께 실행합니다. 브라우저에서 `http://<서버 주소>:8079/`를 여세요. TUI 병행 모드에서는 감지된 웹 모니터 주소(예: `http://192.168.0.177:8079`)가 TUI 오른쪽 위에 표시됩니다. `--localhost` 지정 시 `http://127.0.0.1:8079`로 표시됩니다. TUI 없이 웹 대시보드만 실행하려면 `--no-tui`를 추가합니다. 헤더 오른쪽 카메라 아이콘은 대시보드 이미지 좌우에 10px 여백을 두고, 하단 버전 푸터는 제외해 클립보드에 복사합니다. 클립보드 사용이 브라우저에서 차단되면 이미지 파일을 대신 다운로드합니다. 클립보드 복사에는 HTTPS 또는 localhost 접속이 필요합니다.

대시보드 하단 `ADDITIONAL MONITORS`에 다른 ctop의 주소를 추가하면 GPU LIST는 통합 표시되고, GPU, SYSTEM, GPU PROCESSES는 모니터별 섹션으로 표시됩니다. `192.168.0.177:8079` 또는 `//192.168.0.177:8079`처럼 입력하면 현재 대시보드의 프로토콜(`http`/`https`)을 적용하고 `/api/v1/metrics`를 자동으로 추가합니다. 단독 모니터일 때는 접두어와 입력란을 숨깁니다. 여러 모니터를 표시할 때는 현재 서버가 기본 `A`이고, GPU LIST 제목 옆에서 접두어를 바꿀 수 있습니다. 각 모니터의 `GPU(A)`, `SYSTEM(A)`, `GPU PROCESSES(A)` 섹션은 제목 왼쪽 삼각형 버튼으로 개별 이동할 수 있습니다. GPU, SYSTEM, GPU PROCESSES 섹션은 제목 오른쪽 아이콘으로 각각 접고 펼칠 수 있습니다. GPU LIST도 같은 이동 버튼을 제공하며, 순서는 브라우저에 저장됩니다. 추가 모니터에는 중복되지 않는 접두어(`B`, `C` 등)가 자동 할당되어 GPU 번호가 `A0`, `B0`처럼 표시됩니다. 주소, 접두어, 주소 옆 `Token` 칸에 입력한 토큰은 브라우저의 로컬스토리지에 저장됩니다. 원격 주소는 브라우저에서 접근 가능해야 하며, 원격 서버는 브라우저의 교차 출처 요청을 허용해야 합니다. 토큰 보호된 원격 API에는 `Token` 칸을 사용하세요. metrics URL의 `?token=...` 방식도 지원됩니다.

현재 상태와 그래프 히스토리는 JSON API로도 조회할 수 있습니다.

```bash
curl http://localhost:8079/api/v1/metrics
```

`--token`을 지정하면 `/login`과 아이콘 파일(`/favicon.svg`, `/favicon.ico`)을 제외한 모든 경로가 보호됩니다. 브라우저에서는 첫 접속 시 토큰 입력 화면이 뜨고, 인증 후 7일짜리 쿠키로 유지됩니다. 쿠키가 만료되거나 사라지면 대시보드 안의 입력창에 토큰을 다시 넣어야 합니다. 그 입력창에 넣은 토큰만 로컬스토리지에 저장되어 이후에는 쿠키가 사라져도 자동으로 재인증되며, 로그인 화면에서 입력한 토큰은 저장되지 않습니다. 스크립트에서는 아래 두 방법 중 하나를 씁니다.

```bash
curl -H "Authorization: Bearer 토큰" http://localhost:8079/api/v1/metrics
curl "http://localhost:8079/api/v1/metrics?token=토큰"
```

응답에는 GPU, 프로세스, RAM/SWAP, 디스크, 네트워크의 현재값과 그래프 히스토리가 JSON으로 포함됩니다(`application/json`). `/api/metrics`도 같은 응답을 반환합니다. 히스토리 샘플에는 현재 시점으로부터의 경과 시간(`age_seconds`)과 측정값(`value`)이 포함되며, NET 히스토리의 측정값은 bit/s 처리량이고 샘플 간격은 `sample_interval`(초)로 제공됩니다. 400초 구간의 샘플이 500개를 넘으면 구간별로 축약하되 각 구간의 최솟값과 최댓값은 그대로 남깁니다. GPU 평균은 측정값을 정상적으로 반환한 장치 기준으로 계산합니다. 웹 서버는 기본적으로 모든 네트워크 인터페이스(`0.0.0.0`)에서 연결을 받습니다. 로컬에서만 접속하려면 `--localhost`를 지정하세요. `--token`을 지정하지 않으면 웹 페이지와 API가 인증 없이 열립니다. 다중 모니터링을 위해 metrics API는 교차 출처 브라우저 요청을 허용하므로, 네트워크에 노출하는 인스턴스에는 `--token`을 설정하세요.

## 키 조작

```text
q / Esc                   종료
h                         도움말 표시/숨김
g                         GPU 히스토리 접기/펼치기
m                         SYSTEM 섹션 접기/펼치기
Space                     일시정지/재개
+ / -                     갱신 주기 변경
r                         그래프 히스토리 초기화
j                         다음 GPU 프로세스 선택
k / K                     선택한 프로세스 종료 확인
```

터미널이 SGR 마우스 입력을 지원하면 상단의 조작 항목을 클릭할 수도 있습니다.

## 디스크 표시 방식

SYSTEM 섹션의 디스크 정보는 다음 형식으로 표시합니다.

```text
DISK(XX%) XXX.XGB/YYY.YGB R:읽기값 W:쓰기값
```

`XXX.XGB`는 현재 사용량, `YYY.YGB`는 전체 용량입니다. `R:`과 `W:`에는 각각 읽기 및 쓰기 처리량이 표시됩니다(예: `W:271K`). I/O 그래프는 파티션과 `loop*`, `ram*`, `zram*` 장치를 제외한 블록 장치들의 읽기/쓰기 처리량 합계를 표시합니다. Y축 최고값은 최근 400초 동안 측정된 최대 피크로 자동 설정되며, 0~100% 기준이 아닙니다. 디스크 용량은 루트 파일시스템(`/`) 기준입니다.

## 네트워크 표시 방식

네트워크 카운터의 byte 차이를 bit로 변환하여 표시합니다.

```text
NET | <링크속도>(사용률) | TOTAL:처리량 | ▼:수신 | ▲:송신
```

`/sys/class/net/<interface>/speed`에서 인터페이스별 링크 속도를 읽어 사용률을 계산합니다. 속도를 읽을 수 없는 인터페이스는 사용률을 `N/A`로 표시합니다. Docker 컨테이너가 외부로 통신하는 경우에도 호스트의 물리 NIC를 통과한 트래픽은 호스트 전체 사용량으로 집계됩니다. 단, 컨테이너별 사용량을 따로 구분하지는 않습니다.

## 주의사항

- NVML이 초기화되지 않으면 프로그램이 실행되지 않습니다.
- CUDA Toolkit 전체가 아니라도 `nvml.h`와 NVML stub 라이브러리는 컴파일에 필요합니다.
- NVIDIA 드라이버와 NVML 런타임은 실행 시 필요합니다.
- 네트워크 링크 속도를 읽을 수 없는 인터페이스는 사용률을 계산할 수 없습니다.
