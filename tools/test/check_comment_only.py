"""Checks that the working-tree change to C files is comments only.

  python tools/test/check_comment_only.py [file.c ...]   (default: every modified src/**/*.c)

Each file's code with comments and whitespace removed must equal the same of its HEAD version,
string and character literals kept as they are. Prints one line per file; exit code = number of
files whose code changed.
"""
import re
import subprocess
import sys

TOKEN = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*|\s+|.', re.S)


def code(text):
    out = []
    for m in TOKEN.finditer(text):
        t = m.group(0)
        if t.startswith('/*') or t.startswith('//'):
            out.append(' ')
        elif t.isspace():
            out.append(' ')
        else:
            out.append(t)
    # Whitespace only matters between two word characters.
    s = re.sub(r' +', ' ', ''.join(out))
    return re.sub(r' ?([^\w ]) ?', r'\1', s).strip()


def code_lines(text):
    """Code lines as written (indentation kept), comments and blank lines dropped."""
    out, cur = [], []
    for m in TOKEN.finditer(text):
        t = m.group(0)
        if t.startswith('/*') or t.startswith('//'):
            t = '\n' * t.count('\n')
        parts = t.split('\n')
        cur.append(parts[0])
        for p in parts[1:]:
            out.append(''.join(cur).rstrip())
            cur = [p]
    out.append(''.join(cur).rstrip())
    return [l for l in out if l.strip()]


def main(files):
    if not files:
        diff = subprocess.run(['git', 'diff', '--name-only', 'HEAD', '--', 'src'], capture_output=True, text=True)
        files = [f for f in diff.stdout.split() if f.endswith(('.c', '.h'))]
    bad = 0
    for f in files:
        old = subprocess.run(['git', 'show', 'HEAD:' + f], capture_output=True, text=True, encoding='utf-8')
        with open(f, encoding='utf-8', newline='') as fh:
            new = fh.read()
        if old.returncode != 0:
            print('NEW      ' + f)
            continue
        if code(old.stdout) != code(new):
            verdict = 'CODE CHANGED '
        elif '\r' in new and '\r' not in old.stdout:
            verdict = 'CRLF         '
        elif code_lines(old.stdout) != code_lines(new.replace('\r', '')):
            verdict = 'REFORMATTED  '
        else:
            verdict = 'ok           '
        bad += verdict[0] != 'o'
        print(verdict + f)
    return bad


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
