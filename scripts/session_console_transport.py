"""Shared prompt-driven PL011/QMP transport for the workstation and acceptance test."""
from __future__ import annotations
import json, os, selectors, socket, subprocess, time
from pathlib import Path

def check(condition: bool, detail: str) -> None:
    if not condition:
        raise AssertionError(detail)

class Console:
    def __init__(self, binary: Path, directory: Path, name: str, image: Path | None, address: int, mode: str, shift: int):
        self.transcript = directory / (name + '.log')
        self.log = self.transcript.open('wb')
        self.error = (directory / (name + '.stderr')).open('wb')
        self.qmp_path = directory / (name + '.sock')
        cmd = ['qemu-system-aarch64','-machine','virt','-cpu','cortex-a72','-smp','4','-m','256M',
               '-display','none','-monitor','none','-serial','stdio','-net','none','-kernel',str(binary),
               '-qmp',f'unix:{self.qmp_path},server=on,wait=off']
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
            self.read_prompt()
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
