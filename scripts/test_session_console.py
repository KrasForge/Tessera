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

class Console:
    def __init__(self, binary: Path, directory: Path, name: str, image: Path | None, address: int, mode: str):
        self.transcript = directory / (name + '.log')
        self.log = self.transcript.open('wb')
        self.error = (directory / (name + '.stderr')).open('wb')
        self.qmp_path = directory / (name + '.sock')
        cmd = ['qemu-system-aarch64','-machine','virt','-cpu','cortex-a72','-smp','4','-m','256M',
               '-display','none','-monitor','none','-serial','stdio','-net','none','-kernel',str(binary),
               '-qmp',f'unix:{self.qmp_path},server=on,wait=off']
        if mode == 'deterministic':
            cmd += ['-accel','tcg,thread=single','-icount','shift=3,align=off,sleep=off']
        else:
            cmd += ['-accel','tcg,thread=multi']
        if mode == 'deterministic':
            cmd += ['-accel','tcg,thread=single','-icount',f'shift={shift},align=off,sleep=off']
        else:
            cmd += ['-accel','tcg,thread=multi']
        if image is not None:
            cmd += ['-device',f'loader,file={image},addr={address},force-raw=on']
        (directory / (name+'.command.json')).write_text(json.dumps(cmd,indent=2))
        self.proc = subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.error)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.proc.stdout,selectors.EVENT_READ)
        self.buffer = b''
        try:
            self.boot_text = self.read_prompt()
        except BaseException:
            self.close()
            raise
        self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect(str(self.qmp_path))
        self.qmp=self.sock.makefile('rwb',buffering=0)
        check('QMP' in json.loads(self.qmp.readline()),'QMP greeting')
        self.rpc('qmp_capabilities')
    def read_prompt(self) -> str:
        end=time.monotonic()+15
        marker=b'tessera> '
        while marker not in self.buffer:
            check(time.monotonic()<end,f'no prompt; transcript={self.transcript}')
            events=self.selector.select(0.1)
            for key,_ in events:
                data=os.read(key.fd,65536)
                check(bool(data),f'emulator closed before prompt; {self.transcript}')
                self.log.write(data);self.log.flush();self.buffer+=data
        result,self.buffer=self.buffer.split(marker,1)
        text=result.decode(errors='replace')
        check('SESSION: FAIL' not in text and 'PANIC' not in text,f'kernel failure: {text}')
        return text
    def send(self, line: str, failure: bool=False) -> str:
        self.proc.stdin.write((line+'\n').encode());self.proc.stdin.flush()
        result=self.read_prompt()
        check(('error:' in result)==failure,f'unexpected command result {line!r}: {result}')
        return result
    def rpc(self, name: str, arguments: dict | None=None):
        command={'execute':name}
        if arguments is not None:command['arguments']=arguments
        self.qmp.write(json.dumps(command).encode()+b'\n')
        while True:
            answer=json.loads(self.qmp.readline())
            if 'event' in answer:continue
            check('error' not in answer,f'QMP {name}: {answer}')
            return answer.get('return')
    def finish(self):
        self.send('quit')
        self.proc.stdin.close()
        self.proc.wait(timeout=10)
        tail=self.proc.stdout.read()
        self.log.write(tail);self.log.flush()
        text=self.transcript.read_text()
        check(self.proc.returncode==0 and text.count('WORKSTATION: PASS')==1 and 'SESSION: FAIL' not in text,'clean final verdict')
        self.close()
    def close(self):
        if self.proc.poll() is None:self.proc.kill();self.proc.wait()
        self.selector.close();self.log.close();self.error.close()
        if hasattr(self,'qmp'):self.qmp.close();self.sock.close()

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
    parser.add_argument('--mode',choices=['deterministic','wallclock'],default='wallclock');args=parser.parse_args()
    binary=(args.build_dir/'session/kernel.elf').resolve()
    root=args.build_dir/'m11-m13-validation';root.mkdir(parents=True,exist_ok=True)
    directory=Path(tempfile.mkdtemp(prefix='console-',dir=root)).resolve()
    symbols=subprocess.check_output(['aarch64-linux-gnu-nm','-S',str(binary)],text=True)
    row=next(line.split() for line in symbols.splitlines() if line.endswith(' session_sd'))
    address,size=int(row[0],16),int(row[1],16)
    evidence={'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'boots':2,'mode':args.mode,'cold_boot':True,'storage':'QMP-exported memory-backed FAT image','timing_mode':args.mode,'icount_shift':args.shift if args.mode=='deterministic' else None}
    c=Console(binary,directory,'boot-1',None,address,args.mode)
    try:
        check('contract' in c.send('help'),'contract help')
        check('pid 1' in c.send('load /sd/SOURCE.ELF'),'load source from FAT')
        check('pid 2' in c.send('load /sd/GAIN.ELF'),'load gain from FAT')
        c.send('wire 1 2');c.send('wire 2 dac')
        audio=re.search(r'audio format: sample_rate=(\d+) frames=(\d+)',c.boot_text)
        check(audio is not None,'declared application audio profile')
        rate,frames=map(int,audio.groups())
        evidence['sample_rate']=rate;evidence['frames']=frames
        if frames==240 and rate==48000:
            # Functional 5 ms period: explicit reservations, not hardware WCET.
            c.send('contract 1 hard 800us deadline=4000us policy=mute')
            c.send('contract 2 hard 800us deadline=4800us policy=mute')
        else:
            check(frames==64 and rate==48000,'supported fast diagnostic profile')
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
        continuity=c.send('inspect'); live_stats=c.send('stats');print(continuity+live_stats,flush=True)
        live_counts=re.findall(r'plugin pid=(\d+)(?: cpu=\d+)? runs=(\d+) completed=(\d+) budget=(\d+) deadline=(\d+) shed=(\d+) killed=(\d+)',live_stats)
        check(len(live_counts)==2,'live per-plugin counters')
        if frames==240:
            check(all(all(int(n)==0 for n in row[3:]) for row in live_counts),'live DSP deadline/budget continuity')

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
        c.finish()
    finally:c.close()
    image=bytearray(disk.read_bytes());saved=fat_file(image,'LIVE.TSP')
    check(b'wire 0 1 0' in saved and b'wire 1 dac 0' in saved,'persisted graph')
    fat_add(image,'BAD.TSP',b'# tessera-session v1\naudio broken\n')
    fat_add(image,'PARTIAL.TSP',saved.replace(b'GAIN.ELF',b'LOST.ELF'))
    fat_add(image,'CLOCK.TSP',saved.replace(b'audio 48000 ',b'audio 44100 '))
    disk.write_bytes(image)
    c=Console(binary,directory,'boot-2',disk,address,args.mode)
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
        evidence['wallclock_plugin_counters']=[list(map(int,row)) for row in counters]
        if frames==240:
            check(all(all(int(n)==0 for n in row[3:]) for row in counters),'restored DSP deadline/budget continuity')
        # This serial/FAT correctness test retains and reports timing misses;
        # strict capacity/deadline acceptance is a separate fixture.

        c.send('pause');c.send('clear');c.finish()
    finally:c.close()
    evidence.update({'passed':True,'pcm_reference':reference,'saved_session':saved.decode(),'directory':str(directory)})
    (directory/'result.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print('M13 SERIAL COLD BOOT: PASS; evidence='+str(directory))

if __name__=='__main__':main()
