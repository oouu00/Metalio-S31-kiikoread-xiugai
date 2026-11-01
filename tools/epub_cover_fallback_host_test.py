#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：真实封面解析、优先级、大小预算及原书只读比对。/ English: Actual cover priority, budget and read-only source comparison."""
from pathlib import Path
import subprocess,tempfile,zipfile,sys
R=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "book_epub.h"
int ttf_font_open_mem(uint8_t *data,size_t len,const char *label){(void)len;(void)label;free(data);return -1;}
void ttf_font_close_embedded(void){}
int main(int argc,char **argv){assert(argc==4);uint8_t *p=NULL;size_t n=0;bool png=false;
 size_t budget=(size_t)strtoul(argv[2],NULL,10);int expected=atoi(argv[3]);
 esp_err_t e=book_epub_cover_bounded(argv[1],&p,&n,&png,budget);
 if(expected<0){assert(e!=ESP_OK&&!p&&!n&&!png);return 0;}
 assert(e==ESP_OK&&p&&n<=budget&&png==(expected!=0));
 assert(fwrite(p,1,n,stdout)==n);free(p);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='epub-cover-') as d:
 w=Path(d);(w/'test.c').write_text(unit);exe=w/'test'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(R/'tools/book_epub_stubs'),'-I'+str(R/'tools/zip_host_stubs'),'-I'+str(R/'main/book'),str(w/'test.c')]+[str(R/'main/book'/p) for p in ['book_epub.c','zip_reader.c','html_text.c','book_index_cache.c','book_image_header.c']]+['-lz','-o',str(exe)],check=True)
 def book(name,items,meta='',broken=False):
  p=w/(name+'.epub')
  with zipfile.ZipFile(p,'w',zipfile.ZIP_DEFLATED) as z:
   z.writestr('META-INF/container.xml','<container><rootfiles><rootfile full-path="OEBPS/content.opf"/></rootfiles></container>')
   z.writestr('OEBPS/content.opf','<package><metadata>'+meta+'</metadata><manifest>'+''.join('<item id="'+i+'" href="Images/'+n+'" media-type="'+m+'" '+prop+'/>' for i,n,m,prop in items)+'</manifest></'+('wrong' if broken else 'package')+'>')
   for i,n,m,prop in items:z.writestr('OEBPS/Images/'+n,(i+'-payload').encode())
  return p
 def run(p,payload=None,png=False,budget=4096):
  a=subprocess.run([str(exe),str(p),str(budget),str(int(png) if payload is not None else -1)],capture_output=True,check=True)
  assert a.stdout==(payload or b'')
 fallback=[('ad','logo.jpg','image/jpeg',''),('fallback','CoVeR.PNG','image/png','')]
 p=book('legacy',fallback);run(p,b'fallback-payload',True);run(p,budget=3)
 run(book('declared',fallback+[('official','front.jpg','image/jpeg','properties="cover-image"')]),b'official-payload')
 run(book('epub2',fallback+[('official','front.jpg','image/jpeg','')],'<meta name="cover" content="official"/>'),b'official-payload')
 run(book('no-guess',[('ad','logo.jpg','image/jpeg',''),('slim','Coverslim.jpg','image/jpeg','')]))
 run(book('bad-xml',fallback,broken=True));run(book('mime',[('fallback','cover.bin','image/png','')]),b'fallback-payload',True)
 run(book('traversal',[('bad','../../../../cover.jpg','image/jpeg','')]))
 for path in sys.argv[1:]:
  p=Path(path)
  with zipfile.ZipFile(p) as z:payload=z.read('OEBPS/Images/cover.jpg')
  run(p,payload,budget=len(payload));run(p,budget=len(payload)-1)
 print('PASS: actual EPUB2/3 declaration priority, exact-name fallback, no logo/ad guessing, malformed/path rejection, size limits and read-only original cover bytes')
