import sys,struct,math
from pathlib import Path
root=Path(sys.argv[1]); points=[(1,0,160,120),(2,0,480,120),(3,0,480,360),(4,0,160,360),(5,0,320,240),(7,1,320,240),(8,1,480,360)]
errors=[]
for n,player,x,y in points:
 r=(root/f'ram{n:04d}.bin').read_bytes()
 hit=struct.unpack_from('<ii',r,0x3b5820+player*0x3c)
 error=max(abs(hit[0]-x),abs(hit[1]-y));errors.append(error)
 pointer=struct.unpack_from('<I',r,0x3860f4)[0]&0x1ffffff
 angle=struct.unpack_from('<I',r,pointer+236)[0]
 stock=320/math.tan((angle>>1)*2*math.pi/65536)
 live=struct.unpack_from('<f',r,0xe0600)[0]*320
 print(f'pass{n} P{player+1} ray({x},{y}) -> game{hit}, error{error}px; stock={stock:.3f}, draw={live:.3f}')
 assert 10<live<stock-100 and abs(stock-718.744)<.1
assert max(errors)<=1
print('PASS live-game two-player grid within1px, stock lens retained while drawing widens')
