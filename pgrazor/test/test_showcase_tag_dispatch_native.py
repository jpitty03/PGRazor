"""Runs the production tag builder against a moving managed-heap fixture."""
import test_showcase_tag_runtime_native as guards

class ShowcaseTagDispatchTests(guards.ShowcaseTagRuntimeTests):
    SOURCE = 'showcase_tag_dispatch_test.cpp'
    MARKER = 'PASS tag RPC dispatch'

if __name__ == '__main__':
    import unittest
    unittest.main()
