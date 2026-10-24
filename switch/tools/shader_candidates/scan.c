// Decompress every Yay0 block of the ROM and the raw ROM, find plausible G_SETCOMBINE commands.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static uint32_t be32(const uint8_t*p){return (uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3];}
static uint8_t *rom; static size_t romsz;
static int valid_op(uint8_t o){
    // F3DEX2 + RDP opcodes plausible in a display list
    switch(o){
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x09: case 0x0A: case 0x0B:
    case 0xD7: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED:
    case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF7: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        return 1;}
    return 0;
}
static int rdp_op(uint8_t o){ return o>=0xE4 && o<=0xFF && o!=0xE6+100; }
// legal combiner fields
static int legal(uint32_t w0,uint32_t w1,int *cyc1same){
    int a0=(w0>>20)&15, c0=(w0>>15)&31, Aa0=(w0>>12)&7, Ac0=(w0>>9)&7, a1=(w0>>5)&15, c1=w0&31;
    int b0=(w1>>28)&15, b1=(w1>>24)&15, Aa1=(w1>>21)&7, Ac1=(w1>>18)&7, d0=(w1>>15)&7, Ab0=(w1>>12)&7, Ad0=(w1>>9)&7, d1=(w1>>6)&7, Ab1=(w1>>3)&7, Ad1=w1&7;
    if(a0>7&&a0!=15) return 0; if(a1>7&&a1!=15) return 0;
    if(b0>7&&b0!=15) return 0; if(b1>7&&b1!=15) return 0;
    if(c0>15&&c0<31) return 0; if(c1>15&&c1<31) return 0; // 16..30 are 0 (allowed by HW but never emitted)
    *cyc1same = (a0==a1&&b0==b1&&c0==c1&&d0==d1&&Aa0==Aa1&&Ab0==Ab1&&Ac0==Ac1&&Ad0==Ad1);
    return 1;
}
static void scan(const uint8_t*d,size_t n,const char*tag){
    for(size_t i=8;i+16<=n;i+=8){
        if(d[i]!=0xFC) continue;
        if(!valid_op(d[i-8])||!valid_op(d[i+8])) continue;
        // require at least one more valid neighbour on each side and one neighbour being an RDP-type op
        if(i>=16&&!valid_op(d[i-16])) continue;
        if(i+24<=n&&!valid_op(d[i+16])) continue;
        uint32_t w0=be32(d+i), w1=be32(d+i+4); int same;
        if(!legal(w0,w1,&same)) continue;
        printf("%06X%08X %d %s\n", w0&0xFFFFFF, w1, same, tag);
    }
}
static int yay0(const uint8_t*s,size_t avail,uint8_t**out,size_t*outn){
    if(avail<16) return 0;
    uint32_t size=be32(s+4), lo=be32(s+8), co=be32(s+12);
    if(size==0||size>0x1000000||lo>=avail||co>=avail) return 0;
    uint8_t*o=malloc(size); size_t n=0; uint32_t ctrl=16, link=lo, chunk=co, mask=0; int bits=0;
    while(n<size){
        if(bits==0){ if(ctrl+4>avail){free(o);return 0;} mask=be32(s+ctrl); ctrl+=4; bits=32; }
        if(mask&0x80000000u){ if(chunk>=avail){free(o);return 0;} o[n++]=s[chunk++]; }
        else{
            if(link+2>avail){free(o);return 0;}
            uint32_t pair=(s[link]<<8)|s[link+1]; link+=2;
            uint32_t dist=(pair&0xFFF)+1, cnt=pair>>12;
            if(cnt==0){ if(chunk>=avail){free(o);return 0;} cnt=s[chunk++]+18; } else cnt+=2;
            if(dist>n){free(o);return 0;}
            for(uint32_t k=0;k<cnt&&n<size;k++){ o[n]=o[n-dist]; n++; }
        }
        mask<<=1; bits--;
    }
    *out=o; *outn=size; return 1;
}
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); romsz=ftell(f); fseek(f,0,SEEK_SET); rom=malloc(romsz); fread(rom,1,romsz,f); fclose(f);
    scan(rom,romsz,"raw");
    size_t blocks=0,total=0;
    for(size_t i=0;i+16<=romsz;i+=4){
        if(memcmp(rom+i,"Yay0",4)) continue;
        uint8_t*o; size_t n;
        if(yay0(rom+i,romsz-i,&o,&n)){ char tag[32]; sprintf(tag,"y%zX",i); scan(o,n,tag); free(o); blocks++; total+=n; }
    }
    fprintf(stderr,"yay0 blocks ok: %zu, %zu bytes\n",blocks,total);
    return 0;
}
