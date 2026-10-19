# shared settings for the port's tests (sourced). Windows (Git Bash) unless noted.
PORT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SC=$(cd "$PORT/.." && pwd)
SCW=$(cygpath -m "$SC" 2>/dev/null || echo "$SC")
EXE="$(cygpath -u "$LOCALAPPDATA" 2>/dev/null)/sc-m1/m1.exe"
OUT="$PORT/build/tests"
mkdir -p "$OUT"
# the scripted pad sequence: Start, A ... into an Arcade fight (Kilik)
PADS="600:s,606:-,900:a,906:-,1200:s,1206:-,1500:a,1506:-,1800:s,1806:-,2100:a,2106:-,2400:s,2406:-,2700:a,2706:-"
# PPM -> PNG (Python, no extra modules)
ppm2png() { python -I - "$1" "$2" <<'PY'
import sys, struct, zlib
d=open(sys.argv[1],'rb').read(); hdr=d.split(b'\n',3); w,h=map(int,hdr[1].split()); raw=hdr[3]
rows=b''.join(b'\0'+raw[y*w*3:(y+1)*w*3] for y in range(h))
ch=lambda t,b: struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
open(sys.argv[2],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(rows))+ch(b'IEND',b''))
PY
}
