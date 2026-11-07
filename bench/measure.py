#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate real pinned trees and synthetic search workloads; keep raw time/RSS."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]


def digest(data): return hashlib.sha256(data).hexdigest()


def capture(*command): return subprocess.check_output(command,text=True).strip()


def escape(data,path=False):
    return b''.join((('\\x%02x'%c).encode() if c<32 or c>=127 or c==92 or (path and c==58) else bytes([c])) for c in data)


def oracle(root,suffixes,pattern):
    queue=[b'']; output=bytearray(); manifest=[]; offset=0
    expression=re.compile(pattern)
    while offset<len(queue):
        prefix=queue[offset];offset+=1
        directory=os.fsencode(root)+(b'/'+prefix if prefix else b'')
        for name in sorted(os.listdir(directory)):
            if name.startswith(b'.') or name in (b'vendor',b'target',b'node_modules'): continue
            relative=prefix+b'/'+name if prefix else name
            absolute=directory+b'/'+name
            if os.path.islink(absolute): continue
            if os.path.isdir(absolute): queue.append(relative);continue
            if not os.path.isfile(absolute) or not relative.endswith(suffixes): continue
            data=Path(os.fsdecode(absolute)).read_bytes()
            manifest.append({'path_hex':relative.hex(),'bytes':len(data),'sha256':digest(data)})
            if b'\0' in data: continue
            lines=data.split(b'\n')
            if not data or data.endswith(b'\n'): lines=lines[:-1]
            for number,line in enumerate(lines,1):
                found=expression.search(line)
                if found:
                    output.extend(escape(relative,True)+b':'+str(number).encode()+b':'+str(found.start()+1).encode()+b':'+escape(line)+b'\n')
    return bytes(output),manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose',required=True)
    parser.add_argument('--cc',default='gcc')
    parser.add_argument('--runs',type=int,default=7)
    parser.add_argument('--repos-dir',type=Path,help='Optional goose-libs workspace; read pinned Git objects, never dirty files')
    parser.add_argument('--baseline-ref',help='Optional search CLI commit to build and compare')
    parser.add_argument('--output',type=Path,default=Path('.build/measurements/report.json'))
    args=parser.parse_args()
    if not 1<=args.runs<=100: parser.error('runs must be 1..100')
    compiler=str(Path(args.goose).resolve())
    destination=args.output.resolve();destination.parent.mkdir(parents=True,exist_ok=True)
    binary=ROOT/'.build/benchmark-search'
    commands=[[compiler,'build',str(ROOT/'search.goose'),'--standalone','--cc',args.cc,'-O2','-o',str(binary)]]
    subprocess.run(commands[0],cwd=ROOT,check=True)
    binaries={'candidate':binary}
    baseline=None
    with tempfile.TemporaryDirectory(prefix='goose-search measurement ') as temporary:
        work=Path(temporary)
        if args.baseline_ref:
            baseline=capture('git','-C',str(ROOT),'rev-parse','--verify',args.baseline_ref+'^{commit}')
            source=work/'baseline-source';source.mkdir()
            archive=subprocess.check_output(['git','-C',str(ROOT),'archive',baseline])
            subprocess.run(['tar','-x','-C',str(source)],input=archive,check=True)
            executable=work/'baseline-search'
            command=[compiler,'build',str(source/'search.goose'),'--standalone','--cc',args.cc,'-O2','-o',str(executable)]
            subprocess.run(command,cwd=ROOT,check=True);commands.append(command);binaries['baseline']=executable
        binary_hashes={name:digest(path.read_bytes()) for name,path in binaries.items()}
        cases=[];snapshots=[]
        if args.repos_dir:
            for name,subdir,revision,glob,suffix in [
                ('honk','honk','bb3cb5501d97ded03b792ddb252cc0aaa698f3f5','**/*.goose',(b'.goose',)),
                ('glob','libraries/goose-glob','5543007cc02e324c0fce51919291b40ffb2af922','**/*.goose',(b'.goose',)),
                ('fs','libraries/goose-fs','0d84729aed8d72463349cac7d9439d4039ccd6d3','**/*.[ch]',(b'.c',b'.h'))]:
                directory=work/name;directory.mkdir()
                archive=subprocess.check_output(['git','-C',str(args.repos_dir/subdir),'archive',revision])
                subprocess.run(['tar','-x','-C',str(directory)],input=archive,check=True)
                cases.append((name,directory,glob,suffix,b'return'))
                snapshots.append({'name':name,'revision':revision,'archive_sha256':digest(archive)})
        tiny=work/'tiny';tiny.mkdir()
        for index in range(1000): (tiny/f'{index:04}.txt').write_bytes(b'needle next\n')
        cases.extend([('tiny-hit',tiny,'**/*.txt',(b'.txt',),b'needle'),('tiny-miss',tiny,'**/*.txt',(b'.txt',),b'absent')])
        large=work/'long-lines';large.mkdir()
        for index in range(4): (large/f'{index}.txt').write_bytes((b'needle'+b'x'*(65536-6)+b'\n')*7)
        cases.append(('long-lines',large,'**/*.txt',(b'.txt',),b'^needle'))
        reports=[]
        for name,directory,glob,suffixes,pattern in cases:
            expected,manifest=oracle(directory,suffixes,pattern)
            samples={variant:[] for variant in binaries}
            for executable in binaries.values():
                result=subprocess.run([str(executable),'--glob',glob,'--',os.fsdecode(pattern),str(directory)],capture_output=True,timeout=30)
                if result.returncode!=(0 if expected else 1) or result.stdout!=expected:
                    raise RuntimeError((name,'warmup',result.returncode,result.stderr))
            for iteration in range(args.runs):
                order=list(binaries) if iteration%2==0 else list(reversed(binaries))
                for variant in order:
                    rss=work/'rss.txt'
                    command=['/usr/bin/time','-f','%M','-o',str(rss),str(binaries[variant]),'--glob',glob,'--',os.fsdecode(pattern),str(directory)]
                    start=time.perf_counter_ns();result=subprocess.run(command,capture_output=True,timeout=30);elapsed=time.perf_counter_ns()-start
                    if result.returncode!=(0 if expected else 1) or result.stdout!=expected:
                        raise RuntimeError((name,variant,result.returncode,result.stderr))
                    samples[variant].append({'wall_ns':elapsed,'max_rss_kib':int(rss.read_text().splitlines()[-1]),'exit':result.returncode,'stdout_sha256':digest(result.stdout)})
            reports.append({'name':name,'glob':glob,'regex':pattern.decode(),'input_bytes':sum(p['bytes'] for p in manifest),'files':manifest,'output_bytes':len(expected),'output_sha256':digest(expected),'samples':samples})
        sources=['search.goose','src/searching.goose','native/output.c','search.build','scripts/native-manifest.py','bench/measure.py','vendor/pins.json']
        report={'head':capture('git','-C',str(ROOT),'rev-parse','HEAD'),'dirty':bool(capture('git','-C',str(ROOT),'status','--porcelain','--untracked-files=no')),
                'baseline_commit':baseline,'build_commands':commands,'source_sha256':{name:digest((ROOT/name).read_bytes()) for name in sources},
                'binaries_sha256':binary_hashes,'goose_pin':(ROOT/'GOOSE_REVISION').read_text().strip(),'goose_sha256':digest(Path(compiler).read_bytes()),
                'cc':capture(args.cc,'--version'),'pcre2':capture('pkg-config','--modversion','libpcre2-8'),'platform':platform.platform(),'affinity':sorted(os.sched_getaffinity(0)),
                'snapshots':snapshots,'cases':reports,'limits':['Warm-cache sequential trials on a shared host; no cache flush or speed threshold.',
                'Wall time includes GNU time wrapper, startup, collection, I/O, matching, escaping, stdout writes and parent pipe reads.',
                'Linux GNU time reports child peak RSS in KiB, not cumulative allocations or virtual reservations.',
                'Python byte-regex oracle covers these fixtures, not all PCRE2 syntax or races.']}
        destination.write_text(json.dumps(report,indent=2)+'\n')
    print(destination)


if __name__=='__main__': main()
