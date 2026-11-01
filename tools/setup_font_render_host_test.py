#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Validate an official setup download with the actual firmware rasterizer (host only)."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,sys
root=Path(__file__).resolve().parents[1];font=Path(sys.argv[1]);meta=json.loads((root/'main/font/setup_font.json').read_text())
assert font.stat().st_size==meta['size'] and hashlib.sha256(font.read_bytes()).hexdigest()==meta['sha256']
unit=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
int main(int argc,char**argv){assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>1024);rewind(f);unsigned char*data=malloc((size_t)n);assert(data&&fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);stbtt_fontinfo info;assert(stbtt_InitFont(&info,data,0));const int glyphs[]={0x4f60,0x597d,0x7ee7,0x7eed,0x601d,0x6e90,0x9ed1,0x4f53,'A','g','7'};for(unsigned i=0;i<sizeof(glyphs)/sizeof(glyphs[0]);++i){assert(stbtt_FindGlyphIndex(&info,glyphs[i])!=0);for(int px=22;px<=52;px+=15){int w,h,x,y;float scale=stbtt_ScaleForPixelHeight(&info,(float)px);unsigned char*b=stbtt_GetCodepointBitmap(&info,0,scale,glyphs[i],&w,&h,&x,&y);assert(b&&w>0&&h>0&&w<px*2&&h<px*2);int ink=0;for(int j=0;j<w*h;++j)ink|=b[j];assert(ink);stbtt_FreeBitmap(b,NULL);}}free(data);puts("PASS: pinned Source Han Sans CN, actual firmware stb_truetype renders Chinese/Latin/digits at22/37/52px under ASan/UBSan");}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(unit);exe=p/'test'
 subprocess.run(['cc','-std=c11','-O1','-fsanitize=address,undefined','-I'+str(root/'main/font'),str(p/'test.c'),'-lm','-o',str(exe)],check=True);subprocess.run([str(exe),str(font)],check=True)
