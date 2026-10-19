import sys,glob
from PIL import Image, ImageDraw
ids=[int(x) for x in sys.argv[2:]]
W,H=432,333
cols=3
tiles=[]
for i in ids:
    for s in 'ab':
        try: im=Image.open(f'pd-game-v2/shot_qs_{i}_{s}.png').convert('RGB').resize((W,H))
        except Exception: im=Image.new('RGB',(W,H),(60,0,60))
        ImageDraw.Draw(im).text((6,6),f'{i}{s}',fill=(255,255,0))
        tiles.append(im)
rows=(len(tiles)+cols-1)//cols
sh=Image.new('RGB',(W*cols,H*rows))
for k,t in enumerate(tiles): sh.paste(t,((k%cols)*W,(k//cols)*H))
sh.save(sys.argv[1])
