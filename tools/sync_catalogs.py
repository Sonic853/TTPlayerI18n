#!/usr/bin/env python3
"""Classify PO catalogs and fill Chinese resource translations from ttpres JSON."""
import argparse
import ast
from dataclasses import dataclass, field
import json
from pathlib import Path
import re


@dataclass
class Entry:
    context: str = ''
    source: str = ''
    plural: str = ''
    translations: dict = field(default_factory=dict)
    comments: list = field(default_factory=list)
    obsolete: bool = False

    @property
    def key(self):
        return self.context, self.source, self.plural


def read_po(path):
    entries = []
    entry, active = Entry(), None
    for number, line in enumerate(path.read_text(encoding='utf-8-sig').splitlines() + [''], 1):
        line = line.strip()
        if not line:
            if entry.source:
                entries.append(entry)
            entry, active = Entry(), None
            continue
        if line.startswith('#~ '):
            entry.obsolete = True
            line = line[3:]
        if line.startswith('#'):
            if not line.startswith('# ===') and not line.startswith('#. 资源参照：'):
                entry.comments.append(line)
            continue
        match = re.fullmatch(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[(\d+)\])?)\s+(".*")', line)
        if match:
            name, index, literal = match.groups()
            active = int(index or 0) if name.startswith('msgstr') else {
                'msgctxt': 'context', 'msgid': 'source', 'msgid_plural': 'plural'}[name]
            if isinstance(active, int):
                entry.translations[active] = ''
            else:
                setattr(entry, active, '')
        elif line.startswith('"') and active is not None:
            literal = line
        else:
            raise ValueError(f'{path}:{number}: invalid PO line')
        value = ast.literal_eval(literal)
        if not isinstance(value, str):
            raise ValueError(f'{path}:{number}: expected a PO string')
        if isinstance(active, int):
            entry.translations[active] += value
        else:
            setattr(entry, active, getattr(entry, active) + value)
    keys = [entry.key for entry in entries if not entry.obsolete]
    if len(keys) != len(set(keys)):
        raise ValueError(f'{path}: duplicate active entries')
    return entries


def group(entry):
    for index, (prefix, title) in enumerate([
            ('app', '重建版自建文本'),
            ('exe/string/', '重建版字符串资源'),
            ('exe/dialog/', '重建版对话框'),
            ('exe/menu/', '重建版菜单'),
            ('ttpres/string/', 'ttpres 字符串表'),
            ('ttpres/menu/', 'ttpres 菜单'),
            ('ttpres/dialog/', 'ttpres 对话框')]):
        if entry.context.startswith(prefix):
            return index, title
    return 7, '其他文本'


def quote(value):
    return json.dumps(value, ensure_ascii=False)


def render_entry(entry, forms, reference=None):
    comments = list(dict.fromkeys(entry.comments))
    if reference:
        comments.append('#. 资源参照：' + reference)
    result = '\n'.join(comments) + ('\n' if comments else '')
    result += f'msgctxt {quote(entry.context)}\nmsgid {quote(entry.source)}\n'
    if entry.plural:
        result += f'msgid_plural {quote(entry.plural)}\n'
        result += ''.join(f'msgstr[{i}] {quote(entry.translations.get(i, ""))}\n' for i in range(forms))
    else:
        result += f'msgstr {quote(entry.translations.get(0, ""))}\n'
    if entry.obsolete:
        result = '\n'.join('#~ ' + line for line in result.splitlines()) + '\n'
    return result + '\n'


def resource_entries(path):
    data = json.loads(path.read_text(encoding='utf-8'))
    result = {}
    for entry in data['entries']:
        if entry['category'] not in ('string', 'menu', 'dialog'):
            continue
        key = entry['context']
        if key in result:
            raise ValueError(f'{path}: duplicate resource context {key}')
        result[key] = entry['text']
    return result


def synchronize(template, previous, resources, language):
    names = {'en_US': ('en_US', 'English'), 'chs': ('zh_CN', '简体中文'), 'cht': ('zh_TW', '繁體中文')}
    locale, name = names[language]
    forms = 2 if language == 'en_US' else 1
    formula = '(n != 1)' if forms == 2 else '0'
    header = ('Project-Id-Version: TTPlayerRebuild\n' + f'Language: {locale}\nLanguage-Team: {name}\n' +
              'MIME-Version: 1.0\nContent-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n' +
              f'Plural-Forms: nplurals={forms}; plural={formula};\n')
    result = '# TTPlayer 界面翻译，按语言及文本用途分类。UTF-8。\nmsgid ""\nmsgstr ""\n'
    result += ''.join(quote(line + '\n') + '\n' for line in header.splitlines()) + '\n'
    existing = {entry.key: entry for entry in previous if not entry.obsolete}
    current_keys = {entry.key for entry in template}
    category = None
    translated = resource_count = 0
    for source in sorted(template, key=lambda entry: (group(entry)[0], entry.context, entry.source)):
        title = group(source)[1]
        if title != category:
            result += f'# === {title} ===\n\n'
            category = title
        old = existing.get(source.key)
        entry = Entry(source.context, source.source, source.plural,
                      dict(old.translations) if old else {},
                      [comment for comment in (old.comments if old else [])
                       if not comment.startswith('#:') and comment not in source.comments] + source.comments)
        reference = None
        if source.context in resources:
            reference = f'ttpres/{language}'
            resource_count += 1
            # Preserve translator edits; import only newly added or empty entries.
            if not source.plural and not entry.translations.get(0):
                entry.translations[0] = resources[source.context]
        if all(entry.translations.get(i) for i in range(forms if source.plural else 1)):
            translated += 1
        result += render_entry(entry, forms, reference)
    retired = [entry for entry in previous if entry.key not in current_keys]
    if retired:
        result += '# === 已停用词条（保留历史翻译） ===\n\n'
        for entry in retired:
            entry.obsolete = True
            result += render_entry(entry, max(entry.translations, default=0) + 1)
    return result, translated, resource_count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--i18n', type=Path, default=Path(__file__).resolve().parents[1] / 'i18n')
    parser.add_argument('--ttpres', type=Path, default=Path(__file__).resolve().parents[2] / 'ttpres')
    parser.add_argument('--check', action='store_true', help='Check synchronization without writing files')
    args = parser.parse_args()
    template = [entry for entry in read_po(args.i18n / 'ttplayer.pot') if not entry.obsolete]
    changed = []
    for language in ('en_US', 'chs', 'cht'):
        path = args.i18n / language / 'ttplayer.po'
        previous = read_po(path) if path.exists() else []
        resources = {} if language == 'en_US' else resource_entries(args.ttpres / language / 'texts.json')
        text, translated, imported = synchronize(template, previous, resources, language)
        if not path.exists() or path.read_text(encoding='utf-8') != text:
            changed.append(str(path))
            if not args.check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8', newline='\n')
        print(f'{language}: {translated}/{len(template)} translated; {imported} matching resource entries')
    if args.check and changed:
        raise SystemExit('Catalogs need synchronization:\n' + '\n'.join(changed))


if __name__ == '__main__':
    main()
