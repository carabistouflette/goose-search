#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--tool', required=True)
args, remaining = parser.parse_known_args()
TOOL = str(Path(args.tool).resolve())


def escaped(data, path=False):
    return b''.join((('\\x%02x' % c).encode() if c<32 or c>=127 or c==92 or (path and c==58) else bytes([c])) for c in data)


class Search(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='goose search ')
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def put(self, path, data):
        dest = self.root/path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)

    def run_search(self, pattern='hit', *options, expected=None):
        result = subprocess.run([TOOL, *options, '--', pattern, str(self.root)], capture_output=True, timeout=15)
        if expected is not None:
            self.assertEqual(result.returncode, expected, result.stderr)
        return result

    def test_first_match_per_line_and_breadth_first_order(self):
        self.put('b', b'hit hit\nmiss\nhit')
        self.put('a/deep', b'xxhit\n')
        self.put('a/root', b'hit\n')
        result = self.run_search(expected=0)
        self.assertEqual(result.stdout, b'b:1:1:hit hit\nb:3:1:hit\na/deep:1:3:xxhit\na/root:1:1:hit\n')
        self.assertEqual(result.stderr, b'')

    def test_empty_matches_and_line_boundaries(self):
        for data, expected in [(b'', b''), (b'\n', b'a:1:1:\n'), (b'x\n', b'a:1:1:x\n'),
                               (b'\n\n', b'a:1:1:\na:2:1:\n'), (b'x', b'a:1:1:x\n')]:
            self.put('a', data)
            result = self.run_search('', expected=0 if expected else 1)
            self.assertEqual(result.stdout, expected)
        self.put('a', b'abc\r\n')
        self.assertEqual(self.run_search('$', expected=0).stdout, b'a:1:5:abc\\x0d\n')
        self.assertEqual(self.run_search('c$', expected=1).stdout, b'')

    def test_binary_policy_and_late_nul(self):
        self.put('a', b'hit\n' + b'x'*65537 + b'\0')
        self.assertEqual(self.run_search(expected=1).stdout, b'')
        self.assertEqual(self.run_search('hit', '--binary', 'search', expected=2).stdout, b'')
        self.put('a', b'hit\0\xff\n')
        self.assertEqual(self.run_search('hit', '--binary', 'search', expected=0).stdout, b'a:1:1:hit\\x00\\xff\n')

    def test_arbitrary_path_and_content_bytes_are_escaped(self):
        name = b'colon:back\\newline\n\xff'
        (self.root/os.fsdecode(name)).write_bytes(b'\x1bhit\t\xff\\\n')
        result = self.run_search(expected=0)
        self.assertEqual(result.stdout, escaped(name, True)+b':1:2:'+escaped(b'\x1bhit\t\xff\\')+b'\n')
        self.assertNotIn(b'\x1b', result.stdout)

    def test_hidden_exclusions_and_path_glob(self):
        for name in ['.hidden', 'vendor/a', 'x/vendor/a', 'node_modules/a', 'target/a', 'src/a.goose', 'src/b.txt']:
            self.put(name, b'hit')
        result = self.run_search('hit', '--glob', '**/*.goose', expected=0)
        self.assertEqual(result.stdout, b'src/a.goose:1:1:hit\n')
        self.assertEqual(self.run_search(expected=0).stdout, b'src/a.goose:1:1:hit\nsrc/b.txt:1:1:hit\n')

    def test_symlinks_fifo_and_root_symlink(self):
        outside = self.root/'external'
        outside.write_bytes(b'hit')
        (self.root/'link').symlink_to(outside)
        (self.root/'loop').symlink_to(self.root, target_is_directory=True)
        os.mkfifo(self.root/'pipe')
        self.assertEqual(self.run_search(expected=0).stdout, b'external:1:1:hit\n')
        alias = self.root.parent/(self.root.name+'-link')
        alias.symlink_to(self.root, target_is_directory=True)
        try:
            result = subprocess.run([TOOL,'--','hit',str(alias)],capture_output=True,timeout=5)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,b'')
        finally:
            alias.unlink()

    def test_line_file_and_total_limits_clear_earlier_matches(self):
        self.put('a', b'hit')
        self.put('b', b'x'*65537)
        self.assertEqual(self.run_search(expected=2).stdout,b'')
        self.put('b', b'hit'+b'x'*65533)
        self.assertEqual(self.run_search(expected=0).returncode,0)
        self.put('b', b'\0'*(1048576+1))
        result=self.run_search(expected=2)
        self.assertEqual(result.stdout,b'')
        self.assertIn(b'file byte limit',result.stderr)
        (self.root/'a').unlink()
        (self.root/'b').unlink()
        for i in range(65): self.put(f'{i:02}', b'\0'*1048576)
        result=self.run_search(expected=2)
        self.assertEqual(result.stdout,b'')
        self.assertIn(b'total input',result.stderr)
        (self.root/'64').unlink()
        self.assertEqual(self.run_search(expected=1).stdout,b'')

    def test_match_and_output_limits(self):
        self.put('a',b'hit\n'*10000)
        self.assertEqual(len(self.run_search(expected=0).stdout.splitlines()),10000)
        self.put('a',b'hit\n'*10001)
        self.assertEqual(self.run_search(expected=2).stdout,b'')
        self.put('a',(b'hit'+b'\x1b'*65532+b'\n')*16)
        self.put('b',(b'hit'+b'\x1b'*65532+b'\n')*16)
        self.put('c',(b'hit'+b'\x1b'*65532+b'\n')*16)
        result=self.run_search(expected=2)
        self.assertEqual(result.stdout,b'')
        self.assertIn(b'output limit',result.stderr)

    def test_call_and_collection_limits(self):
        self.put('a',b'\n'*1000000)
        self.assertEqual(self.run_search('x',expected=1).stdout,b'')
        self.put('a',b'\n'*1000001)
        result=self.run_search('x',expected=2)
        self.assertEqual(result.stdout,b'')
        self.assertIn(b'call limit',result.stderr)
        (self.root/'a').unlink()
        for i in range(1001): self.put(str(i),b'')
        self.assertEqual(self.run_search(expected=2).stdout,b'')

    def test_regex_failures_and_invalid_arguments(self):
        self.put('a',b'hit')
        self.put('b',b'a'*128+b'!')
        limited=self.run_search('(*NO_START_OPT)(?:hit|(a+)+$)',expected=2)
        self.assertEqual(limited.stdout,b'')
        self.assertIn(b'regex search failed',limited.stderr)
        for pattern in ['(', '(*UTF)x', 'x'*16385]:
            self.assertEqual(self.run_search(pattern,expected=2).stdout,b'')
        self.assertEqual(self.run_search('hit','--glob','a'*1024,expected=1).stdout,b'')
        for options in [('--glob','a'*1025),('--glob','['),('--binary','bad'),('--glob','**','--glob','**')]:
            self.assertEqual(self.run_search('hit',*options,expected=2).stdout,b'')
        for command in [[TOOL],[TOOL,'hit',str(self.root)],[TOOL,'--','hit','relative']]:
            self.assertEqual(subprocess.run(command,capture_output=True,timeout=5).returncode,2)

    def test_stdout_failures(self):
        self.put('a',b'hit')
        with open('/dev/full','wb') as full:
            result=subprocess.run([TOOL,'--','hit',str(self.root)],stdout=full,stderr=subprocess.PIPE,timeout=5)
        self.assertEqual(result.returncode,2)
        self.assertIn(b'stdout write',result.stderr)
        read,write=os.pipe()
        os.close(read)
        try:
            result=subprocess.run([TOOL,'--','hit',str(self.root)],stdout=write,stderr=subprocess.PIPE,timeout=5)
            self.assertIn(result.returncode,[-signal.SIGPIPE,2])
        finally: os.close(write)

    def test_python_byte_regex_oracle(self):
        data=b'\nabc\r\nxxab\xff\naba\naaa\nlast'
        self.put('a',data)
        lines=data.split(b'\n')
        patterns=[b'',b'a',b'a+',b'(a)?b',b'(?<=a)b',b'(?=a)',b'a|ab',b'$',b'^',b'[a-z]+',b'\xff']
        for pattern in patterns:
            expected=b''
            for index,line in enumerate(lines,1):
                found=re.search(pattern,line)
                if found: expected+=b'a:'+str(index).encode()+b':'+str(found.start()+1).encode()+b':'+escaped(line)+b'\n'
            result=self.run_search(os.fsdecode(pattern),expected=0 if expected else 1)
            self.assertEqual(result.stdout,expected,pattern)


if __name__=='__main__':
    unittest.main(argv=[__file__,*remaining])
