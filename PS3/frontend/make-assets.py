from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import sys
out=Path(sys.argv[1]); (out/'USRDIR').mkdir(parents=True,exist_ok=True)
font=Path(__file__).resolve().parents[2]/'assets/Inconsolata-Regular.ttf'
f=ImageFont.truetype(str(font),20)
im=Image.new('RGB',(192,144),'black'); d=ImageDraw.Draw(im)
for c in range(32,127): d.text((((c-32)%16)*12,((c-32)//16)*24),chr(c),font=f,fill='white')
im.save(out/'USRDIR/font.bmp')
# Twice the size for menu titles (24x48 cells).
f=ImageFont.truetype(str(font),40)
im=Image.new('RGB',(384,288),'black'); d=ImageDraw.Draw(im)
for c in range(32,127): d.text((((c-32)%16)*24,((c-32)//16)*48),chr(c),font=f,fill='white')
im.save(out/'USRDIR/font_large.bmp')
# ICON0.PNG (320x176, the XMB icon): drawn at twice the size, then scaled down for smooth edges.
# Original artwork (no console or controller imagery): a dark gradient, speed stripes, the name.
assets=Path(__file__).resolve().parents[2]/'assets'
bold=str(assets/'Roboto_Condensed-Bold.ttf')
W,H=640,352
im=Image.new('RGB',(W,H))
px=im.load()
import math
for y in range(H):
    for x in range(W):
        t=(x/W*0.6+y/H*0.4)
        r=int(14+40*t); g=int(18+8*t); b=int(48+70*t)
        # A soft glow behind the name.
        dx=(x-W*0.42)/(W*0.55); dy=(y-H*0.45)/(H*0.6)
        glow=max(0.0,1.0-(dx*dx+dy*dy))
        r=min(255,int(r+40*glow)); g=min(255,int(g+70*glow)); b=min(255,int(b+90*glow))
        px[x,y]=(r,g,b)
d=ImageDraw.Draw(im,'RGBA')
# Speed stripes from the right edge.
for i,(y0,th,a) in enumerate([(70,10,90),(104,6,70),(250,8,80),(282,5,60),(306,3,50)]):
    x0=W-60-i*70
    d.polygon([(x0,y0),(W+40,y0),(W+40-th*2,y0+th),(x0-th*2,y0+th)],fill=(80,220,255,a))
# Thin scan lines.
for y in range(0,H,4):
    d.line([(0,y),(W,y)],fill=(0,0,0,26))
# The name: slanted forward, white to cyan, with a glow.
f=ImageFont.truetype(bold,150)
mask=Image.new('L',(W,H),0); ImageDraw.Draw(mask).text((46,40),'PPSSPP',font=f,fill=255)
shear=0.18
mask=mask.transform((W,H),Image.AFFINE,(1,shear,-shear*H*0.35,0,1,0),resample=Image.BICUBIC)
from PIL import ImageFilter
glow=mask.filter(ImageFilter.GaussianBlur(18))
glowcol=Image.new('RGB',(W,H),(60,190,255))
im.paste(glowcol,(0,0),glow.point(lambda v:int(v*0.85)))
grad=Image.new('RGB',(W,H))
gp=grad.load()
for y in range(H):
    t=max(0.0,min(1.0,(y-60)/150))
    c=(int(255-120*t),int(255-25*t),255)
    for x in range(W):
        gp[x,y]=c
im.paste(grad,(0,0),mask)
d=ImageDraw.Draw(im,'RGBA')
# The badge under the name: "for PS3" (it only said "PS3" up to build 267).
fb=ImageFont.truetype(bold,64)
bx,by=40,236
tw=d.textlength('for PS3',font=fb)
d.rounded_rectangle([bx,by,bx+tw+40,by+80],radius=16,fill=(40,210,255,255))
d.text((bx+20,by+2),'for PS3',font=fb,fill=(10,20,45,255))
im=im.resize((320,176),Image.LANCZOS)
im.save(out/'ICON0.PNG')
