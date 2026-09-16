#!/usr/bin/env python3
"""M13 acceptance over the real QEMU PL011, including a SECOND emulator boot.
The block image is exported/imported by QMP; this is not physical SD testing.
No retries: every command, emulator exit and restored sample is checked.
"""
from __future__ import annotations
import argparse, hashlib, json, os, re, selectors, socket, struct, subprocess, tempfile, time
from pathlib import Path

def check(condition: bool, detail: str) -> None:
    if not condition:
        raise AssertionError(detail)

from session_console_transport import Console

def fat_file(image: bytearray, name: str) -> bytes:
    reserved=struct.unpack_from('<H',image,14)[0];fats=image[16];spf=struct.unpack_from('<H',image,22)[0]
    entries=struct.unpack_from('<H',image,17)[0];root=(reserved+fats*spf)*512
    data=root+((entries*32+511)//512)*512
    base,ext=name.split('.');key=(base.ljust(8)+ext.ljust(3)).encode()
    for offset in range(root,root+entries*32,32):
        if image[offset:offset+11]!=key:continue
        cluster=struct.unpack_from('<H',image,offset+26)[0];size=struct.unpack_from('<I',image,offset+28)[0]
        out=b'';seen=set()
        while len(out)<size:
            check(cluster>=2 and cluster not in seen,'FAT chain');seen.add(cluster)
            out+=image[data+(cluster-2)*512:data+(cluster-1)*512]
            cluster=struct.unpack_from('<H',image,reserved*512+2*cluster)[0]
        return out[:size]
    raise AssertionError('file missing: '+name)

def fat_add(image: bytearray, name: str, payload: bytes) -> None:
    reserved=struct.unpack_from('<H',image,14)[0];fats=image[16];spf=struct.unpack_from('<H',image,22)[0]
    entries=struct.unpack_from('<H',image,17)[0];root=(reserved+fats*spf)*512;data=root+((entries*32+511)//512)*512
    slot=next(i for i in range(root,root+entries*32,32) if image[i] in (0,229))
    free=[c for c in range(2,(len(image)-data)//512+2) if struct.unpack_from('<H',image,reserved*512+2*c)[0]==0]
    count=(len(payload)+511)//512;check(len(free)>=count,'FAT free space')
    chain=free[:count]
    for i,c in enumerate(chain):
        struct.pack_into('<H',image,reserved*512+2*c,chain[i+1] if i+1<count else 0xffff)
        chunk=payload[i*512:(i+1)*512];off=data+(c-2)*512;image[off:off+512]=chunk.ljust(512,b'\0')
    base,ext=name.split('.');image[slot:slot+32]=bytes(32);image[slot:slot+11]=(base.ljust(8)+ext.ljust(3)).encode();image[slot+11]=32
    struct.pack_into('<H',image,slot+26,chain[0]);struct.pack_into('<I',image,slot+28,len(payload))

def sample(text: str) -> tuple[str,int,int]:
    m=re.search(r'hash=([0-9a-f]+) left=(-?\d+) right=(-?\d+)',text)
    check(m is not None,'sample inspection')
    return m[1],int(m[2]),int(m[3])

def main() -> None:
    parser=argparse.ArgumentParser();parser.add_argument('--build-dir',type=Path,default=Path('build/arm'))
    parser.add_argument('--mode',choices=['deterministic','wallclock'],default='deterministic')
    parser.add_argument('--shift',type=int,choices=range(0,5),default=0)
    args=parser.parse_args()
    binary=(args.build_dir/'session/kernel.elf').resolve()
    root=args.build_dir/'m11-m13-validation';root.mkdir(parents=True,exist_ok=True)
    directory=Path(tempfile.mkdtemp(prefix='console-',dir=root)).resolve()
    symbols=subprocess.check_output(['aarch64-linux-gnu-nm','-S',str(binary)],text=True)
    row=next(line.split() for line in symbols.splitlines() if line.endswith(' session_sd'))
    address,size=int(row[0],16),int(row[1],16)
    evidence={'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'boots':2,'mode':args.mode,'cold_boot':True,'storage':'QMP-exported memory-backed FAT image','timing_mode':args.mode,'icount_shift':args.shift if args.mode=='deterministic' else None}
    c=Console(binary,directory,'boot-1',None,address,args.mode,args.shift)
    try:
        check('contract' in c.send('help'),'contract help')
        check('pid 1' in c.send('load /sd/SOURCE.ELF'),'load source from FAT')
        check('pid 2' in c.send('load /sd/GAIN.ELF'),'load gain from FAT')
        c.send('wire 1 2');c.send('wire 2 dac')
        c.send('contract 1 hard 400us deadline=1000us policy=mute')
        c.send('contract 2 hard 400us deadline=1200us policy=mute')
        c.send('pin 1 2');c.send('pin 2 1');c.send('set-param 2 0 0.5')
        c.send('cores 3',True);c.send('start');c.send('wait 128')
        inspection=c.send('inspect')
        reference=sample(inspection);check(reference[1:]==(8192,-8192),'actual reference PCM')
        before_missing=int(re.search(r'missing=(\d+)',inspection)[1])
        check(before_missing==0 and 'watchdog=0' in inspection,'initial continuity')
        # Live topology, loading, admission and core reassignment preserve the
        # existing signal. No pause commands, replacement in a frame boundary.
        for iteration in range(3):
            added=c.send('load /sd/GAIN.ELF')
            extra=int(re.search(r'loaded pid (\d+)',added)[1])
            c.send(f'wire 1 {extra}');c.send(f'unwire 1 {extra}')
            c.send(f'unload {extra}')
            c.send('pin 1 1');c.send('pin 1 2')
        c.send('wait 32')
        check(sample(c.send('inspect'))==reference,'live edits retain exact signal')
        continuity=c.send('inspect');print(continuity+c.send('stats'),flush=True)
        check(int(re.search(r'missing=(\d+)',continuity)[1])==before_missing,'live edits dropped audio')
        check('watchdog=0' in continuity,'live edits exceeded audio IRQ budget')
        c.send('unload 4294967297',True)
        c.send('contract 1 hard 18446744073709551615us',True)
        c.send('contract 1 hard budget=100us budget=200us',True)
        c.send('wire 2 1',True)
        c.send('clear '+' '*160+'trailing',True)
        view=c.send('ls');check('pid=1' in view and 'pid=2' in view,'rejected input retained graph')
        c.send('patch save /sd/LIVE.TSP')
        c.send('patch load /sd/MISSING.TSP',True)
        c.send('pause');check('LIVE.TSP' in c.send('patch ls'),'saved session listed')
        disk=directory/'card.img';c.rpc('pmemsave',{'val':address,'size':size,'filename':str(disk)})
        check(disk.stat().st_size==size,'complete persistent image')
        c.send('pause');c.send('clear')
        a=int(re.search(r'loaded pid (\d+)',c.send('load /sd/SYNTH.ELF'))[1])
        b=int(re.search(r'loaded pid (\d+)',c.send('load /sd/FILTER.ELF'))[1])
        c.send(f'wire {a} {b}');c.send(f'wire {b} dac')
        c.send('start');c.send('wait 64')
        tone=sample(c.send('inspect'));check(tone[1:]!=(0,0),'synth-filter output is silent')
        c.send('pause');c.send('clear')
        check('session-ok' in c.send('echo session-ok'),'echo command')
        c.send('ls\b\bhelp')
        c.finish()
    finally:c.close()
    image=bytearray(disk.read_bytes());saved=fat_file(image,'LIVE.TSP')
    check(b'wire 0 1 0' in saved and b'wire 1 dac 0' in saved,'persisted graph')
    fat_add(image,'BAD.TSP',b'# tessera-session v1\naudio broken\n')
    fat_add(image,'PARTIAL.TSP',saved.replace(b'GAIN.ELF',b'LOST.ELF'))
    fat_add(image,'CLOCK.TSP',saved.replace(b'audio 48000 ',b'audio 44100 '))
    disk.write_bytes(image)
    c=Console(binary,directory,'boot-2',disk,address,args.mode,args.shift)
    try:
        check('pid=' not in c.send('ls'),'cold boot has no previous processes')
        c.send('patch load /sd/LIVE.TSP');view=c.send('ls')
        check('cpu=2 hard' in view and 'cpu=1 hard' in view,'affinity and criticality restored')
        c.send('start');c.send('wait 64')
        observed=sample(c.send('inspect'));check(observed==reference,'identical audio after emulator restart')
        for bad in ['BAD.TSP','PARTIAL.TSP','CLOCK.TSP']:
            c.send('patch load /sd/'+bad,True)
            check('pid=1' in c.send('ls') and 'pid=2' in c.send('ls'),'failed restore kept old PIDs')
            c.send('wait 8');check(sample(c.send('inspect'))==reference,'failed restore retained audio')
        c.send('set-param 2 0 0.25');c.send('wait 16')
        check(sample(c.send('inspect'))[1:]==(4096,-4096),'live queued parameter reaches real plugin')
        stats=c.send('stats')
        counters=re.findall(r'plugin pid=(\d+)(?: cpu=\d+)? runs=(\d+) completed=(\d+) budget=(\d+) deadline=(\d+) shed=(\d+) killed=(\d+)',stats)
        check(len(counters)==2,'both live deadline/budget counters exposed')
        evidence['plugin_counters']=[list(map(int,row)) for row in counters]
        check(all(int(row[2])>0 and all(int(x)==0 for x in row[3:]) for row in counters),'restored graph timing/policy failures')
        final_inspect=c.send('inspect')
        check('missing=0' in final_inspect and 'watchdog=0' in final_inspect,'second boot continuity failed')

        c.send('pause');c.send('clear');c.finish()
    finally:c.close()
    evidence.update({'passed':True,'pcm_reference':reference,'saved_session':saved.decode(),'directory':str(directory)})
    (directory/'result.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print('M13 SERIAL COLD BOOT: PASS; evidence='+str(directory))

if __name__=='__main__':main()
