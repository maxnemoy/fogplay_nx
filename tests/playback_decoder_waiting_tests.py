"""Real Player replaces the former presentation-body splice; all eight cases."""
from player_test_build import ROOT, run_player
run_player((ROOT/'tests/player_decoder_waiting_tests.cpp').read_text())