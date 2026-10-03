import sys, wave, struct, math
w=wave.open(sys.argv[1]); n=w.getnframes(); ch=w.getnchannels(); sw=w.getsampwidth()
data=w.readframes(n)
if sw==2: s=struct.unpack('<%dh'%(len(data)//2),data); scale=32768
else: s=struct.unpack('<%df'%(len(data)//4),data); scale=1
skip=len(s)//4  # ignore start
s=s[skip:]
rms=math.sqrt(sum(x*x for x in s)/max(1,len(s)))/scale
print("%.4f"%rms)
