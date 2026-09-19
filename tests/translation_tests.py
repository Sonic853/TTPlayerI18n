#!/usr/bin/env python3
"""Check shipped translations and safe synchronization of translator edits."""
from pathlib import Path
import re
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import sync_catalogs as sync

TOKENS = re.compile(r'%\([^)]*\)|%%|%[-+ #0.\d*]*(?:hh|ll|I32|I64|[hljztLIw])?[diuoxXfFeEgGaAcCsSpn]|%.?')


class TranslationTests(unittest.TestCase):
    def test_extraction_reuses_pot_without_generating_host_text_table(self):
        template = ROOT / 'i18n/ttplayer.pot'
        expected = {entry.key for entry in sync.read_po(template) if entry.context.startswith(('ttpres/', 'exe/'))}
        with tempfile.TemporaryDirectory(prefix='ttp-i18n-extract-') as directory:
            rebuild = Path(directory) / 'rebuild'
            source = rebuild / 'src/app/main.cpp'
            source.parent.mkdir(parents=True)
            source.write_text('i18n::Literal(L"Current code text");\n', encoding='utf-8')
            output = Path(directory) / 'ttplayer.pot'
            command = [sys.executable, '-X', 'utf8', str(ROOT / 'tools/extract_catalog.py'),
                       '--rebuild', str(rebuild), '--resource-template', str(template), '--output', str(output)]
            subprocess.run(command, check=True, capture_output=True)
            keys = {entry.key for entry in sync.read_po(output)}
            self.assertEqual(expected | {('app', 'Current code text', '')}, keys)
            self.assertFalse((rebuild / 'src/i18n/resource_messages.inc').exists())
            previous = output.read_bytes()
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(previous, output.read_bytes())

    def test_chinese_coverage_and_format_contracts(self):
        template = {entry.key for entry in sync.read_po(ROOT / 'i18n/ttplayer.pot')}
        for language in ('chs', 'cht'):
            entries = sync.read_po(ROOT / f'i18n/{language}/ttplayer.po')
            active = [entry for entry in entries if not entry.obsolete]
            self.assertEqual(template, {entry.key for entry in active})
            for entry in active:
                with self.subTest(language=language, context=entry.context, source=entry.source):
                    self.assertEqual({0}, set(entry.translations))
                    translated = entry.translations[0]
                    self.assertTrue(translated)
                    self.assertFalse(any(comment.startswith('#,') and 'fuzzy' in comment for comment in entry.comments))
                    self.assertEqual(TOKENS.findall(entry.source), TOKENS.findall(translated))
                    for separator in ('|', '\n', '\t'):
                        self.assertEqual(entry.source.count(separator), translated.count(separator))
                    self.assertEqual(re.findall(r'\*\.[\w*]+', entry.source), re.findall(r'\*\.[\w*]+', translated))

    def test_resource_imports_and_catalog_order_are_current(self):
        template = sync.read_po(ROOT / 'i18n/ttplayer.pot')
        for language in ('en_US', 'chs', 'cht'):
            path = ROOT / f'i18n/{language}/ttplayer.po'
            entries = sync.read_po(path)
            resource_path = ROOT.parent / f'ttpres/{language}/texts.json'
            # Resource exports are optional in a standalone checkout. Coverage
            # and format validation above always run against shipped catalogs.
            if language != 'en_US' and not resource_path.exists():
                continue
            resources = {} if language == 'en_US' else sync.resource_entries(resource_path)
            rendered, _, _ = sync.synchronize(template, entries, resources, language)
            self.assertEqual(path.read_text(encoding='utf-8'), rendered)
            for entry in entries:
                if entry.obsolete or entry.context not in resources:
                    continue
                expected = resources[entry.context]
                if language == 'cht' and entry.context == 'ttpres/dialog/254/control/4294967295/4':
                    # The host treats this documented example as a named token.
                    expected = expected.replace('%(欄位名)', '%(字段名)')
                with self.subTest(language=language, context=entry.context):
                    self.assertEqual(expected, entry.translations[0])

    def test_sync_preserves_edits_fuzzy_flags_and_retired_translations(self):
        template = [sync.Entry('ttpres/string/1', 'Original', comments=['#: source:1', '#, c-format']),
                    sync.Entry('ttpres/string/2', 'New'), sync.Entry('app', 'Code')]
        previous = [sync.Entry('ttpres/string/1', 'Original', translations={0: 'Translator edit'},
                               comments=['# Translator note', '#, fuzzy']),
                    sync.Entry('app', 'Retired', translations={0: 'Keep history'})]
        resources = {'ttpres/string/1': 'Resource text', 'ttpres/string/2': 'Imported', 'ttpres/string/99': 'Unused'}
        rendered, _, _ = sync.synchronize(template, previous, resources, 'cht')
        with tempfile.TemporaryDirectory(prefix='ttp-i18n-sync-') as directory:
            path = Path(directory) / 'ttplayer.po'
            path.write_text(rendered, encoding='utf-8')
            parsed = sync.read_po(path)
            entries = {entry.source: entry for entry in parsed}
            self.assertEqual('Translator edit', entries['Original'].translations[0])
            self.assertIn('#, fuzzy', entries['Original'].comments)
            self.assertIn('# Translator note', entries['Original'].comments)
            self.assertEqual('Imported', entries['New'].translations[0])
            self.assertEqual('', entries['Code'].translations[0])
            self.assertTrue(entries['Retired'].obsolete)
            self.assertEqual('Keep history', entries['Retired'].translations[0])
            self.assertEqual(4, len(entries))
            again, _, _ = sync.synchronize(template, parsed, resources, 'cht')
            self.assertEqual(rendered, again)


if __name__ == '__main__':
    unittest.main()
