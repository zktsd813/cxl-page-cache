# cxl-page-cache

Bloom filter로 위치를 찾는 CXL pooled memory 기반 공유 page cache 연구. 여러 **host**가 같은 page를 공유한다(VM이 아니다). 핵심 문서는 [docs/design.md](docs/design.md) 하나다. agent·VM 연구(`~/agent-sandbox-cxl`)와는 별개 프로젝트이고, 그쪽은 이전 구현의 출처로만 링크한다.

- `linux/`: upstream `weiny2/linux-kernel` `dcd-v3-2023-10-30`을 수정 없이 가져온 commit 위에 이 프로젝트의 변경을 얹는다. upstream 원본과의 차이는 git log로 본다.
- 빌드는 out-of-tree(`make -C linux O=../build`), NUMA 0에서. config는 `kernel/config-cxl-page-cache`.

## 역할 분담

Claude Code가 계획·판정·검토를 맡고, 구현과 실행은 이 서버에 로그인된 Codex CLI(`codex exec`)에 맡긴다.
- 지시는 `.codex-runs/NN-<이름>.md` brief로 쓰고 `codex exec -C <repo> -s workspace-write --json -o .codex-runs/NN.last.md - < brief > .codex-runs/NN.jsonl`로 실행한다. 보고서는 `.codex-runs/NN-<이름>.report.md`. `.codex-runs/`는 Git에 넣지 않는다.
- Codex 결과의 숫자는 원시 자료에서 다시 확인한 뒤 문서에 넣는다. commit은 검토 후 Claude가 하고, push는 사용자 허락을 받는다.

## 서버

실행 대상은 Niagara CXL 장치에 연결된 서버다. 코드 작성과 컴파일 확인은 bud05에서 했다. 아래는 bud05에서 작업할 때의 규칙이다.

### bud05 규칙 (hostname `ubuntu`)

- NUMA 0(CPU 0-31, sibling 64-95, 메모리 node 0)만 쓴다. NUMA 1은 다른 사용자 작업이 있어 쓰지 않는다.
- node 2는 host 4대가 공유하는 CXL DCD(128 GB, 2 GB block)다. 용량은 `dcdctl --mbox=remote --mbox-remote=http://143.248.188.99`로만 받는다. memory block을 직접 online하지 않는다. `dcdctl add/release`는 사용자 확인 후에만. `info` 같은 읽기 조회는 괜찮다.
- 실험 데이터, Docker image, 다른 사용자의 파일을 지우지 않는다.

## 문서 규칙

한국어, 짧은 문장. 측정하지 않은 값은 추정이라고 적는다.
