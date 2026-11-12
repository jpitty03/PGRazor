"""Production host state-machine fixture; managed graph tests are separate."""
import test_showcase_tag_runtime_native as harness

class ShowcaseHostTests(harness.ShowcaseTagRuntimeTests):
    SOURCE='showcase_host_test.cpp'
    MARKER='PASS Showcase host phases'

if __name__=='__main__':
    import unittest
    unittest.main()
