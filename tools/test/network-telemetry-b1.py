#!/usr/bin/env python3
"""Exercise unchanged B1 parser on bytes emitted by real typed record classes."""
import importlib.util,json,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools/diagnostics'))
spec=importlib.util.spec_from_file_location('network_b1_report',root/'tools/diagnostics/stage0-report.py')
report=importlib.util.module_from_spec(spec);spec.loader.exec_module(report)
parsed=report.parse_log(Path(sys.argv[1]))
run=parsed['runs'][0];session=run['streams'][0]['nack']
assert run['diagnostic_clock']['status']=='exact'
assert session['latest_final']['nackPackets']==2
assert session['latest_final']['nackArrivalBucketMs']==10
assert len(session['expired_hole_events'])==1
entry=session['expired_hole_events'][0]
assert entry['seq']==101 and entry['normalized_ms']==190 and entry['logging_delay_ms']==60
assert session['counter_reset_boundaries']
Path(sys.argv[2]).write_text(json.dumps(parsed,indent=2)+'\n')
print('Actual typed record bytes -> unchanged B1 clock/event/counters/reset parser passed')
