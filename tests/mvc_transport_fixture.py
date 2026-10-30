"""Wrap the pinned original MVC structural vector in a two-PID MPEG-TS."""
import re
from pathlib import Path

def write_fixture(source:Path,destination:Path):
    streams=[[],[]];groups=[[],[]];access=-1
    def pps_id(nal):
        bits=''.join(f'{b:08b}' for b in nal[1:]);leading=len(bits)-len(bits.lstrip('0'))
        return int(bits[leading:leading*2+1],2)-1
    for nal in re.split(b'\x00\x00\x00?\x01',source.read_bytes()):
        if not nal:continue
        kind=nal[0]&31
        if kind==14:
            access+=1
            if access and (groups[0] or groups[1]):
                for i in range(2):streams[i].append(b''.join(groups[i]));groups[i]=[]
        view=1 if kind in (15,20) or (kind==8 and pps_id(nal)!=0) else 0
        groups[view].append(b'\0\0\0\1'+nal)
    for i in range(2):streams[i].append(b''.join(groups[i]))
    def crc(data):
        value=0xffffffff
        for b in data:
            value^=b<<24
            for _ in range(8):value=((value<<1)^0x04c11db7)&0xffffffff if value&0x80000000 else value<<1&0xffffffff
        return value.to_bytes(4,'big')
    packets=[];continuity={}
    def transport(pid,payload):
        first=True
        while payload:
            n=min(184,len(payload));data=payload[:n];payload=payload[n:]
            cc=continuity.get(pid,0);continuity[pid]=(cc+1)&15
            header=bytes([0x47,(0x40 if first else 0)|(pid>>8),pid&255,(0x10 if n==184 else 0x30)|cc]);first=False
            if n<184:
                padding=183-n
                header+=bytes([padding])+(b'\0'+b'\xff'*(padding-1) if padding else b'')
            packets.append(header+data)
    pat=bytes.fromhex('00 b0 0d 00 01 c1 00 00 00 01 e1 00')
    pmt=bytes.fromhex('02 b0 17 00 01 c1 00 00 f0 11 f0 00 1b f0 11 f0 00 20 f0 12 f0 00')
    transport(0,b'\0'+pat+crc(pat));transport(0x100,b'\0'+pmt+crc(pmt))
    def timestamp(value):return bytes([0x21|((value>>29)&14),(value>>22)&255,((value>>14)&254)|1,(value>>7)&255,((value<<1)&254)|1])
    for frame in range(len(streams[0])):
        for view in range(2):
            data=streams[view][frame];pts=90000+frame*3750
            pes=b'\0\0\1\xe0'+(len(data)+8).to_bytes(2,'big')+b'\x80\x80\x05'+timestamp(pts)+data
            transport(0x1011+view,pes)
    destination.write_bytes(b''.join(packets))
