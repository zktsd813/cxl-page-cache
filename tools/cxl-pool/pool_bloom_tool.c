#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Fixed-size, single-writer, approximate discovery slots. Never pointers or
 * lifetime authority. Stale/partially published bits may lose opportunities.
 * This executable only addresses the controller-reserved 3GiB Bloom area. */
#define SLOT_BYTES 65536U
#define BLOOM_BASE (UINT64_C(3) << 30)
#define MAX_SLOTS 64U
#define HASHES 7U
static void fail(const char *s) { fprintf(stderr,"pool-bloom-tool: %s (%s)\n",s,strerror(errno)); exit(1); }
static void need(int ok,const char *s) { if(!ok) { errno=EINVAL; fail(s); } }
static uint64_t number(const char *s) {
    char *end; need(s && isdigit((unsigned char)*s),"unsigned integer"); errno=0;
    uint64_t n=strtoull(s,&end,10); need(!errno && !*end,"integer syntax"); return n;
}
static uint64_t mix(uint64_t n) {
    n^=n>>30; n*=UINT64_C(0xbf58476d1ce4e5b9); n^=n>>27;
    n*=UINT64_C(0x94d049bb133111eb); return n^(n>>31);
}
static uint64_t image_hash(const char *s) {
    need(strlen(s)==64,"image SHA256 must have 64 hex characters");
    uint64_t h=UINT64_C(14695981039346656037);
    for(unsigned i=0;i<64;i++) { need(isxdigit((unsigned char)s[i]),"image SHA256 hex"); h=(h^(unsigned char)tolower((unsigned char)s[i]))*UINT64_C(1099511628211); }
    return h;
}
static int contains(const unsigned char *p,uint64_t seed,uint64_t key,int insert) {
    uint64_t a=mix(seed^key),b=mix(seed+key+UINT64_C(0x9e3779b97f4a7c15))|1;
    int hit=1;
    for(unsigned i=0;i<HASHES;i++) { unsigned bit=(unsigned)((a+i*b)%(SLOT_BYTES*8));
        if(insert) ((unsigned char *)p)[bit/8]|=(unsigned char)(1U<<(bit%8));
        else if(!(p[bit/8]&(1U<<(bit%8)))) hit=0;
    }
    return hit;
}
static uint64_t capacity(void) {
    FILE *f=fopen("/sys/bus/dax/devices/dax0.0/size","r"); char s[80];
    if(!f || !fgets(s,sizeof(s),f)) fail("DAX capacity");
    if(fclose(f)) fail("capacity close");
    s[strcspn(s,"\n")]=0; return number(s);
}
int main(int argc,char **argv) {
    need(argc==7,"usage: pool-bloom-tool build|probe|zero|ones /dev/dax0.0 SLOT IMAGE_SHA KEYS_FILE OUTPUT_FILE");
    int build=!strcmp(argv[1],"build"),probe=!strcmp(argv[1],"probe"),zero=!strcmp(argv[1],"zero"),ones=!strcmp(argv[1],"ones");
    need(build||probe||zero||ones,"operation"); need(!strcmp(argv[2],"/dev/dax0.0"),"reserved DAX device");
    uint64_t slot=number(argv[3]),seed=image_hash(argv[4]); need(slot<MAX_SLOTS,"slot range");
    uint64_t off=BLOOM_BASE+slot*SLOT_BYTES,cap=capacity(); need(off<cap && SLOT_BYTES<=cap-off,"slot capacity");
    int d=open(argv[2],(probe?O_RDONLY:O_RDWR)|O_CLOEXEC|O_NOFOLLOW); if(d<0) fail("open DAX");
    struct stat st; if(fstat(d,&st)) fail("DAX stat"); need(S_ISCHR(st.st_mode),"DAX character device");
    unsigned char *p=mmap(NULL,SLOT_BYTES,PROT_READ|(probe?0:PROT_WRITE),MAP_SHARED,d,(off_t)off);
    if(p==MAP_FAILED) fail("map Bloom slot");
    unsigned char local[SLOT_BYTES]; uint64_t keys=0,hits=0;
    if(probe) {
        /* Explicit refresh for the coherent lab; physical CXL needs platform
         * validated cache maintenance. Correctness never relies on these bits. */
        for(size_t i=0;i<SLOT_BYTES;i+=64) __asm__ volatile("clflush (%0)"::"r"(p+i):"memory");
        __asm__ volatile("mfence":::"memory"); memcpy(local,p,SLOT_BYTES);
    } else memset(local,ones?255:0,SLOT_BYTES);
    FILE *out=fopen(argv[6],"wx"); if(!out) fail("create output exclusively");
    if(build||probe) {
        FILE *in=fopen(argv[5],"r"); if(!in) fail("open keys"); char line[80];
        while(fgets(line,sizeof(line),in)) {
            need(strchr(line,'\n')!=NULL,"bounded key line"); line[strcspn(line,"\n")]=0;
            uint64_t key=number(line); need(key%4096==0,"page-aligned key"); keys++;
            if(contains(local,seed,key,build)) { hits++; if(probe && fprintf(out,"%" PRIu64 "\n",key)<0) fail("write hits"); }
        }
        if(ferror(in)||fclose(in)) fail("read keys");
    }
    if(!probe) {
        memcpy(p,local,SLOT_BYTES);
        for(size_t i=0;i<SLOT_BYTES;i+=64) __asm__ volatile("clflush (%0)"::"r"(p+i):"memory");
        __asm__ volatile("mfence":::"memory");
    }
    if(fclose(out)||munmap(p,SLOT_BYTES)||close(d)) fail("close resources");
    printf("POOL_BLOOM_JSON {\"operation\":\"%s\",\"slot\":%" PRIu64 ",\"keys\":%" PRIu64 ",\"hits\":%" PRIu64 ",\"slot_bytes\":%u,\"hashes\":%u}\n",argv[1],slot,keys,hits,SLOT_BYTES,HASHES);
    return 0;
}
