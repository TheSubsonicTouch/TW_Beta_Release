import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import model_manager as m


class ManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.path = self.root / 'state.json'
        self.backup = self.root / 'backup.json'
        self.data = m.state.default_state()
        m.atomic_json(self.path, self.data)
        self.look = {'kind': 'look', 'id': 'swat-pointman'}
        self.guard = lambda: None

    def tearDown(self):
        self.temp.cleanup()

    def test_apply_multiple_and_restore_preserves_progress(self):
        original = copy.deepcopy(self.data['player']['CustomizationBag'])
        m.apply(self.look, self.path, self.backup, self.guard)
        first = m.read_json(self.path)
        self.assertNotEqual(first['player']['CustomizationBag'], original)
        self.assertTrue(first['player']['InventoryBag'])
        saved_backup = self.backup.read_bytes()
        m.apply({'kind': 'look', 'id': 'ghost'}, self.path, self.backup, self.guard)
        self.assertEqual(saved_backup, self.backup.read_bytes())
        after = m.read_json(self.path)
        after['player']['CurrencyBag']['Gold'] = '987'
        m.atomic_json(self.path, after)
        m.restore(self.path, self.backup, self.guard)
        restored = m.read_json(self.path)
        self.assertEqual(restored['player']['CustomizationBag'], original)
        self.assertEqual(restored['player']['CurrencyBag']['Gold'], '987')
        self.assertEqual(restored['player']['InventoryBag'], after['player']['InventoryBag'])

    def test_unknown_and_running_refused_without_changes(self):
        before = self.path.read_bytes()
        with self.assertRaises(ValueError):
            m.apply({'kind': 'look', 'id': 'unknown'}, self.path, self.backup, self.guard)
        self.assertFalse(self.backup.exists())
        def running(): raise RuntimeError('server running')
        with self.assertRaises(RuntimeError): m.apply(self.look, self.path, self.backup, running)
        self.assertEqual(before, self.path.read_bytes())

    def test_foreign_backup_is_never_overwritten_or_restored(self):
        m.atomic_json(self.backup, {'account': 'other', 'customization': {}})
        before = self.path.read_bytes()
        with self.assertRaises(ValueError): m.apply(self.look, self.path, self.backup, self.guard)
        with self.assertRaises(ValueError): m.restore(self.path, self.backup, self.guard)
        self.assertEqual(before, self.path.read_bytes())

    def test_folder_validation(self):
        mods = self.root / 'Mods'
        for name, obj in [('good', {'name': 'Test', 'look': 'ghost'}),
                          ('bad', {'name': 'Bad', 'look': 'fake'}),
                          ('extra', {'name': 'X', 'look': 'ghost', 'file': '../../evil'})]:
            d = mods / name; d.mkdir(parents=True)
            (d / 'model.json').write_text(json.dumps(obj), encoding='utf-8')
        entries, errors = m.catalogue(mods)
        self.assertTrue(any(x['name'] == 'Test' for x in entries))
        self.assertEqual(len(errors), 2)

    def test_game_outfit(self):
        uid = next(uid for uid in m.playermodel.catalogue()['items']
                   if m.playermodel.catalogue()['pieces'].get(uid))
        m.apply({'kind': 'outfit', 'id': uid}, self.path, self.backup, self.guard)
        self.assertEqual(m.read_json(self.path)['player']['CustomizationBag']['ClothingOutfitCortexUID'], uid)

    def test_guard_runs_before_read_or_write(self):
        with patch.object(m, 'guard_offline', side_effect=RuntimeError('busy')):
            with self.assertRaises(RuntimeError):
                m.apply(self.look, self.path, self.backup, m.guard_offline)
        self.assertFalse(self.backup.exists())

if __name__ == '__main__':
    unittest.main()
