"""Real Player worker admission/AUTO matrix; both compiled defaults."""
from player_test_build import ROOT, run_player
source=(ROOT/'tests/player_admission_tests.cpp').read_text()
for enabled in (0,1):
    run_player(source,('-DFOGPLAY_MTS_MEDIA_WORKERS='+str(enabled),))