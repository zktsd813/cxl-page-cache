# cxl-page-cache

여러 host가 같은 읽기 전용 파일 page를 CXL pooled memory에 한 벌만 두고 같이 쓰는 공유 page cache. 다른 host에 같은 page가 있는지는 Bloom filter로 찾는다.

- 설계: [docs/design.md](docs/design.md)
- kernel: `linux/` (upstream dcd-v3-2023-10-30 + 이 프로젝트의 변경)
- 도구: `tools/cxl-pool/`
