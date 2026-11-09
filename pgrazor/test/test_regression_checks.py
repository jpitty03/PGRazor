import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/regression'))
import checks


class CheckTests(unittest.TestCase):
    def bridge(self):
        bridge = Mock()
        bridge.snapshot.return_value = {'pid':'42','settings':{'joystickVisible':False}}
        return bridge

    def test_inventory_does_not_require_changed_counts_but_requires_new_read(self):
        bridge = self.bridge()
        state = dict(bridge.snapshot.return_value, inventory={'1':4})
        for revisions, expected in [((1,2),'passed'),((1,1),'failed'),((2,1),'failed')]:
            bridge.inventory.side_effect = [dict(state,inventoryRevision=n) for n in revisions]
            self.assertEqual(checks.execute(bridge,'inventory')['status'],expected)

    def test_restarted_process_cannot_pass(self):
        bridge = self.bridge()
        state = dict(bridge.snapshot.return_value,inventory={'1':4})
        bridge.inventory.side_effect = [dict(state,inventoryRevision=1),dict(state,inventoryRevision=2,pid='43')]
        self.assertEqual(checks.execute(bridge,'inventory')['status'],'failed')

    def test_backup_requires_every_reported_assertion(self):
        bridge = self.bridge()
        for restored, expected in [(True,'passed'),(False,'failed')]:
            bridge.backup.return_value = dict(bridge.snapshot.return_value,backup={'ok':True,'restored':restored})
            self.assertEqual(checks.execute(bridge,'backup')['status'],expected)


if __name__ == '__main__':
    unittest.main()
