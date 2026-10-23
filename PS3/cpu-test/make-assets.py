from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import sys
out=Path(sys.argv[1]); (out/'USRDIR').mkdir(parents=True,exist_ok=True)
font=Path(__file__).resolve().parents[2]/'assets/Inconsolata-Regular.ttf'
f=ImageFont.truetype(str(font),20)
im=Image.new('RGB',(192,144),'black'); d=ImageDraw.Draw(im)
for c in range(32,127): d.text((((c-32)%16)*12,((c-32)//16)*24),chr(c),font=f,fill='white')
im.save(out/'USRDIR/font.bmp')
im=Image.new('RGB',(320,176),(12,18,28));d=ImageDraw.Draw(im);f=ImageFont.truetype(str(font),30)
d.text((22,28),'PPSSPP PS3',font=f,fill='white');d.text((22,80),'CPU TEST 07',font=f,fill=(90,240,140));im.save(out/'ICON0.PNG')
