import sys
sys.path.insert(0,'.')
from png import png, px
img=png(sys.argv[1]); w,h,ch,p=img
x0,y0,x1,y1=[int(v) for v in sys.argv[2:6]]
cols=int(sys.argv[6]) if len(sys.argv)>6 else 80
rows=max(1,int(cols*(y1-y0)/(x1-x0)*0.5))
ramp=" .:-=+*#%@"
for r in range(rows):
    line=''
    for c in range(cols):
        X=x0+int((c+0.5)*(x1-x0)/cols); Y=y0+int((r+0.5)*(y1-y0)/rows)
        R,G,B=px(img,X,Y)
        l=(0.299*R+0.587*G+0.114*B)/255
        line+=ramp[min(9,int(l*11))]
    print(line)
