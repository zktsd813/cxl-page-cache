# CXL 공유 page cache 설계

2026-11-10. 여러 host가 같은 읽기 전용 파일을 읽으면 page cache에 같은 page가 host마다 한 벌씩 생긴다. 이 프로젝트는 그 page를 CXL pooled memory에 한 벌만 두고 모든 host가 같이 쓰게 한다. 다른 host에 같은 page가 있는지는 Bloom filter로 찾는다.

## 1. 구성 요소

| 요소 | 하는 일 | 코드 |
|---|---|---|
| canonical frame | CXL에 둔 불변 데이터 page 한 벌. key는 `(파일 identity와 version, 4 KiB offset)`이다. 게시한 뒤에는 쓰지 않는다 | |
| Bloom slot | host마다 CXL에 고정 크기 slot이 하나 있다. 그 host가 등록한 key를 넣는다. 다른 host는 이 slot을 읽어 후보를 좁힌다 | `tools/cxl-pool/pool_bloom_tool.c` |
| exact index | controller가 혼자 관리한다. key → canonical frame과 READY 상태 | (이전 구현은 Python controller, 아직 옮기지 않음) |
| page cache 등록 | host kernel이 지정한 읽기 전용 ext4 파일의 resident·clean·uptodate·order-0 folio를 찾아 참조와 lock을 잡고 목록을 내보낸다. 목록 digest와 상태 page를 게시한다 | `linux/drivers/misc/cxl_pagecache_reg.c` |
| CXL pool mapper | 연속된 가상주소 하나에 canonical frame(읽기 전용)과 private CXL frame을 섞어 매핑한다. 쓰기 fault가 나면 private frame에 4 KiB를 복사한다(CoW). 내용을 버린 page는 복사 없이 private zero frame으로 바꾼다(discard) | `linux/drivers/dax/cxl_pool.c` |
| devDAX private mapping | devDAX가 4 KiB private mapping과 CoW를 허용하게 한다. 기본 꺼짐(`device_dax.pool_cow=1`) | `linux/drivers/dax/device.c` |

Bloom은 힌트다. false positive는 exact index 조회에서 걸러진다. false negative는 공유 기회를 놓치고 private 사본을 유지할 뿐이다. Bloom만으로 frame의 주소·소유권·수명을 정하지 않는다.

## 2. 공유 자격

page를 공유하려면 다음을 모두 만족해야 한다.

1. 같은 불변 파일 identity(image digest, 파일시스템 UUID, inode와 generation)의 같은 file page다.
2. 등록 시점에 folio가 그 inode의 mapping/index에 속하고, 읽기가 끝났고, clean이고, writeback 중이 아니다.
3. 파일 offset이 고정된 extent 정보와 맞는다. 등록하는 동안 folio를 잠가 둔다.
4. 상태 page의 session cookie, epoch, 등록 목록 digest가 controller가 받은 목록과 일치한다.

dirty bit가 0이라는 사실이나 과거 읽기 기록만으로는 공유하지 않는다. 내용이 우연히 같은 page를 찾는 content dedup은 하지 않는다.

## 3. 흐름

1. **게시**: host A가 파일을 읽어 page cache에 올린다. 등록 driver가 folio 목록을 내보내고, controller가 canonical frame에 내용을 쓴다. write-back을 마친 뒤 READY로 표시하고, A의 Bloom slot에 key를 넣는다.
2. **조회**: host B가 같은 파일을 읽으려 하면 다른 host의 Bloom slot에서 후보를 찾는다. 그다음 exact index에서 READY인 canonical frame을 확인한다.
3. **연결**: B는 그 frame을 읽기 전용으로 매핑한다. 로컬 page cache 사본을 만들지 않는다.
4. **쓰기**: 공유 page에 쓰면 쓴 쪽만 private CXL frame을 받는다(CoW). canonical frame은 다른 host를 위해 그대로 둔다.
5. **회수**: 참조가 남아 있는 canonical frame은 회수하지 않는다. private frame과 남은 credit은 controller에 반환한다.

## 4. 비일관 CXL에서 지킬 것

host 사이에 하드웨어 cache coherence가 없다고 가정한다.

| 문제 | 처리 |
|---|---|
| 게시한 내용이 다른 host에 보이는가 | 쓴 host가 cache line을 write-back하고 fence한 뒤 READY를 쓴다. 읽는 host는 처음 읽기 전에 invalidate한다 |
| canonical 내용이 바뀌는가 | 게시 후 불변이다. 쓰기는 모두 CoW로 private frame에 간다 |
| Bloom slot을 오래된 값으로 읽음 | 허용한다. 공유를 놓치거나 추가 조회가 생길 뿐이다 |
| frame 할당·회수 | controller 하나가 소유권을 정한다. host마다 받은 credit 안에서만 private frame을 쓴다 |
| 여러 host의 동시 CAS | 장치가 host 사이 atomic을 보장하는지 따로 확인해야 한다. 보장이 없으면 controller가 네트워크로 직렬화한다 |

## 5. 열린 문제

- **host page cache와의 연결 지점**: mapper는 지금 프로세스 가상주소에 frame을 매핑한다. host kernel의 page cache(address_space)가 CXL frame을 직접 가리키게 할지, DAX 파일 매핑으로 노출할지, mmap 경로로 쓸지를 정해야 한다. 이 부분이 host 설계의 핵심이다.
- **온라인 게시와 회수**: 이전 구현은 image 전체를 canonical 영역에 미리 적재했다. 필요한 page만 채우고 회수하는 경로가 없다.
- **실제 장비 검증**: bud05의 CXL DCD(host 4대 공유)에서 게시·invalidate 순서, stale 읽기 negative test, host 사이 atomic을 확인해야 한다.
- **비용**: CXL에서 page cache를 읽을 때 느려지는 정도와, host당 줄어드는 메모리를 아직 재지 않았다.

## 6. 코드

- `linux/`: upstream `weiny2/linux-kernel`의 `dcd-v3-2023-10-30`(6.6.0-rc6 + CXL DCD v3)을 수정 없이 가져온 뒤 위 변경을 얹었다. 변경 내역은 git log로 본다.
- `tools/cxl-pool/`: Bloom slot tool과 등록 CLI.
- `kernel/config-cxl-page-cache`: 컴파일 확인에 쓴 config.

이전 구현과 검증 기록은 [agent-sandbox-cxl `809a70d`](https://github.com/zktsd813/agent-sandbox-cxl/tree/809a70d/implementation/virtual_machine)에 있다. 그때는 같은 코드가 microVM 안에서 돌았다.
