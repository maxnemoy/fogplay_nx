"""Compile exact application telemetry method bodies in a platform-free shell."""
import sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
text=(root/'src/features/streaming/presentation/playback_controller.cpp').read_text()
result='namespace fogplay {\n'
for name,args in [('emitPendingFinalPsTelemetry',''),('transportClosed',''),('logOutputTimings','bool final'),('pumpStream','PadState& pad,u64 held,u64 down'),('report','fog::stream::PlayerDiagnosticPhase phase,const fog::stream::PlayerDiagnosticSnapshot& captured')]:
    signature='void ApplicationController::'+name+'('+args+')'
    start=text.index(signature)
    brace=text.index('{',start);end=brace+1;depth=1
    while depth:
        if text[end]=='{':depth+=1
        elif text[end]=='}':depth-=1
        end+=1
    result+=text[start:end]+'\n'
result+='}\n'
Path(sys.argv[1]).write_text(result)
