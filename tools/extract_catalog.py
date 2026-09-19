#!/usr/bin/env python3
"""Extract resource captions and explicitly marked C++ messages into gettext POT.

Without --ttpres/--exe, reuse the checked-in canonical resource table. Resource
extraction loads PE files as data only and never executes their code.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import re
import struct
from sync_catalogs import read_po


def resources(path, kind):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p, wintypes.LPARAM)
    kernel.EnumResourceNamesW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, callback_type, wintypes.LPARAM]
    kernel.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = wintypes.HANDLE
    kernel.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.LoadResource.restype = wintypes.HANDLE
    kernel.LockResource.argtypes = [wintypes.HANDLE]
    kernel.LockResource.restype = ctypes.c_void_p
    module = kernel.LoadLibraryExW(str(path.resolve()), None, 2)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    names = []
    callback = callback_type(lambda m, t, n, p: (names.append(n), True)[1])
    try:
        kernel.EnumResourceNamesW(module, kind, callback, 0)
        for name in names:
            if name > 0xffff:
                continue
            resource = kernel.FindResourceW(module, name, kind)
            size = kernel.SizeofResource(module, resource)
            pointer = kernel.LockResource(kernel.LoadResource(module, resource))
            if not pointer:
                raise RuntimeError(f'Cannot read resource {kind}/{name}')
            yield name, ctypes.string_at(pointer, size)
    finally:
        kernel.FreeLibrary(module)


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def number(self, fmt):
        value = struct.unpack_from('<' + fmt, self.data, self.at)[0]
        self.at += struct.calcsize('<' + fmt)
        return value

    def field(self):
        first = self.number('H')
        if first == 0xffff:
            return self.number('H')
        words = []
        while first:
            words.append(first)
            first = self.number('H')
        return struct.pack('<' + 'H' * len(words), *words).decode('utf-16-le')


def dialog(data, prefix):
    r = Reader(data)
    extended = data[:4] == b'\x01\x00\xff\xff'
    style = struct.unpack_from('<I', data, 12 if extended else 0)[0]
    count = struct.unpack_from('<H', data, 16 if extended else 8)[0]
    r.at = 26 if extended else 18
    r.field(); r.field()
    yield prefix + '/title', r.field()
    if style & 0x40:
        r.at += 6 if extended else 2
        r.field()
    occurrences = {}
    for _ in range(count):
        r.at = (r.at + 3) & ~3
        style = struct.unpack_from('<I', data, r.at + (8 if extended else 0))[0]
        ident = struct.unpack_from('<I' if extended else '<H', data, r.at + (20 if extended else 16))[0]
        if ident == 0xffff:
            ident = 0xffffffff
        occurrence = occurrences.get(ident, 0)
        occurrences[ident] = occurrence + 1
        r.at += 24 if extended else 18
        klass, caption = r.field(), r.field()
        label = klass == 0x80 or (klass == 0x82 and style & 0x1f not in (3, 14)) or (
            isinstance(klass, str) and klass.lower() in ('button', 'static', 'syslink'))
        if label:
            yield f'{prefix}/control/{ident}/{occurrence}', caption
        extra = r.number('H')
        if extra:
            r.at += extra if extended else extra - 2
    if r.at > len(data):
        raise ValueError('Dialog exceeds resource bounds')


def menu(data, prefix):
    r = Reader(data)
    version, offset = r.number('H'), r.number('H')
    if version != 0:
        raise ValueError('Unsupported MENUEX resource')
    r.at += offset

    def children(parent=''):
        index = 0
        while True:
            flags = r.number('H')
            if not flags & 0x10:
                r.number('H')
            path = f'{parent}.{index}' if parent else str(index)
            yield prefix + '/item/' + path, r.field()
            if flags & 0x10:
                yield from children(path)
            index += 1
            if flags & 0x80:
                break
    yield from children()


def extract(path, domain):
    for ident, data in resources(path, 6):
        r = Reader(data)
        for index in range(16):
            size = r.number('H') * 2
            text = data[r.at:r.at + size].decode('utf-16-le')
            r.at += size
            yield f'{domain}/string/{(ident - 1) * 16 + index}', text
    for ident, data in resources(path, 5):
        yield from dialog(data, f'{domain}/dialog/{ident}')
    for ident, data in resources(path, 4):
        yield from menu(data, f'{domain}/menu/{ident}')


def quote(text):
    return json.dumps(text, ensure_ascii=False)


def write_changed(path, text):
    if not path.exists() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rebuild', type=Path, default=Path(__file__).resolve().parents[2] / 'rebuild')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1] / 'i18n/ttplayer.pot')
    parser.add_argument('--resource-template', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'i18n/ttplayer.pot',
                        help='Reuse resource msgids from this POT when no resource binary is supplied')
    parser.add_argument('--ttpres', type=Path)
    parser.add_argument('--exe', type=Path)
    parser.add_argument('--resource-texts', type=Path, action='append', default=[],
                        help='Supplement missing string IDs from a ttpres texts.json export')
    args = parser.parse_args()
    messages = {}
    if args.resource_template.exists():
        for entry in read_po(args.resource_template):
            if entry.obsolete or not entry.context.startswith(('ttpres/', 'exe/')):
                continue
            if entry.context in messages:
                raise ValueError(f'Duplicate resource context: {entry.context}')
            messages[entry.context] = entry.source
    elif not (args.ttpres or args.exe or args.resource_texts):
        parser.error('No resource template found; supply --resource-template, --ttpres, --exe or --resource-texts')
    for domain, path in [('ttpres', args.ttpres), ('exe', args.exe)]:
        if path:
            messages = {key: value for key, value in messages.items() if not key.startswith(domain + '/')}
            messages.update({key: text for key, text in extract(path, domain)
                             if isinstance(text, str) and text and not text.startswith(('http://', 'https://'))})
    for path in args.resource_texts:
        for entry in json.loads(path.read_text(encoding='utf-8'))['entries']:
            key, value = entry['context'], entry['text']
            # Other exports include control class names, URLs and complete
            # embedded files. Only add string-table messages the host can read.
            if entry['category'] == 'string' and re.fullmatch(r'ttpres/string/\d+', key) and \
                    value and not value.startswith(('http://', 'https://')):
                messages.setdefault(key, value)
    entries = {(key, text, ''): [] for key, text in messages.items()}
    literal = r'L("(?:[^"\\]|\\.)*")'
    for source in sorted((args.rebuild / 'src').rglob('*.cpp')) + sorted((args.rebuild / 'src').rglob('*.h')):
        if source.parts[-2] == 'i18n':
            continue
        text = source.read_text(encoding='utf-8-sig')
        for found in re.finditer(r'i18n::(?:Literal|Text)\(\s*' + literal + r'(?:\s*,\s*("[^"\n]*"))?', text):
            key = (json.loads(found[2]) if found[2] else 'app', json.loads(found[1]), '')
            entries.setdefault(key, []).append(f'{source.relative_to(args.rebuild).as_posix()}:{text.count(chr(10), 0, found.start()) + 1}')
        for found in re.finditer(r'i18n::Plural\(\s*("[^"]*")\s*,\s*' + literal + r'\s*,\s*' + literal, text):
            key = tuple(json.loads(found[i]) for i in (1, 2, 3))
            entries.setdefault(key, []).append(f'{source.relative_to(args.rebuild).as_posix()}:{text.count(chr(10), 0, found.start()) + 1}')
    output = ['# TTPlayer interface translations. UTF-8.\n', 'msgid ""\nmsgstr ""\n',
              '"Project-Id-Version: TTPlayerRebuild\\n"\n',
              '"MIME-Version: 1.0\\n"\n', '"Content-Type: text/plain; charset=UTF-8\\n"\n',
              '"Content-Transfer-Encoding: 8bit\\n"\n\n']
    for (context, text, plural), locations in sorted(entries.items()):
        if locations:
            output.append('#: ' + ' '.join(locations) + '\n')
        if '|' in text or '\n' in text:
            output.append('#. Preserve separators, file patterns and format placeholders.\n')
        if re.search(r'%(?:[-+ #0-9.*]*)(?:ll|l|h|I64|I32|z)?[diuoxXfFeEgGaAcCsSp]', text):
            output.append('#, c-format\n')
        output += [f'msgctxt {quote(context)}\n', f'msgid {quote(text)}\n']
        if plural:
            output += [f'msgid_plural {quote(plural)}\n', 'msgstr[0] ""\nmsgstr[1] ""\n\n']
        else:
            output.append('msgstr ""\n\n')
    destination = args.output
    destination.parent.mkdir(parents=True, exist_ok=True)
    write_changed(destination, ''.join(output))
    print(f'{len(messages)} resource entries; {len(entries)} total messages -> {destination}')


if __name__ == '__main__':
    main()
