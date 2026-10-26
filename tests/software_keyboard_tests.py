"""Run the production keyboard helper with deterministic libnx stubs; no device."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/platform/switch/switch_runtime.cpp').read_text()
start = source.index('std::optional<std::string> keyboard(')
brace = source.index('{', start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
method = source[start:end]
stubs = r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
using u64=uint64_t; using u32=uint32_t; using Result=u32;
constexpr u64 HidNpadButton_A=1,HidNpadButton_B=2,HidNpadButton_X=4,HidNpadButton_Y=8,HidNpadButton_Plus=16,HidNpadButton_Minus=32;
constexpr int AppletFocusState_InFocus=1,Module_Libnx=345,LibnxError_LibAppletBadExit=99;
constexpr Result MAKERESULT(int module,int description){return module+(description<<9);}
struct PadState{}; struct SwkbdConfig{};
std::vector<std::string> logs; std::vector<u64> held;
size_t position=0; int shows=0,closes=0,sleeps=0; bool focused=true,passwordPreset=false;
Result createRc=0,showRc=0;
void diagnostic(std::string message){logs.push_back(message);}
void padInitializeDefault(PadState*){}
void padUpdate(PadState*){}
u64 padGetButtons(PadState*){return position<held.size()?held[position++]:0;}
bool appletMainLoop(){return true;}
int appletGetFocusState(){return focused?AppletFocusState_InFocus:0;}
void svcSleepThread(long long){++sleeps;}
void checkResult(Result rc,const char* message){if(rc)throw std::runtime_error(message);}
Result swkbdCreate(SwkbdConfig*,int){return createRc;}
void swkbdConfigMakePresetPassword(SwkbdConfig*){passwordPreset=true;}
void swkbdConfigMakePresetDefault(SwkbdConfig*){}
void swkbdConfigSetHeaderText(SwkbdConfig*,const char*){}
void swkbdConfigSetInitialText(SwkbdConfig*,const char*){}
void swkbdConfigSetStringLenMax(SwkbdConfig*,u32){}
void swkbdConfigSetReturnButtonFlag(SwkbdConfig*,bool){}
Result swkbdShow(SwkbdConfig*,char* result,size_t){++shows; assert(position>=held.size());std::strcpy(result,"synthetic-private-value");return showRc;}
void swkbdClose(SwkbdConfig*){++closes;}
std::optional<std::string> keyboard(const char*,const std::string&,int,bool=false);
void reset(){logs.clear();held.clear();position=0;shows=closes=sleeps=0;focused=true;passwordPreset=false;createRc=showRc=0;}
'''
cases = r'''
int main(){
 reset(); assert(keyboard("Login","",128)=="synthetic-private-value"); assert(shows==1&&closes==1);
 reset();held={HidNpadButton_B,0};assert(keyboard("Login","",128));assert(sleeps==1&&shows==1);
 reset();assert(keyboard("Password","",256,true));assert(passwordPreset);
 for(auto& message:logs)assert(message.find("synthetic-private-value")==std::string::npos);
 reset();showRc=77;bool failed=false;try{keyboard("Login","",128);}catch(...){failed=true;}assert(failed&&closes==1);
 assert(logs.back()=="Keyboard show result=77");
 reset();createRc=88;failed=false;try{keyboard("Login","",128);}catch(...){failed=true;}assert(failed&&shows==0&&closes==0);
 reset();showRc=MAKERESULT(Module_Libnx,LibnxError_LibAppletBadExit);assert(!keyboard("Login","",128));assert(closes==1);
 reset();held={HidNpadButton_B};focused=false;assert(!keyboard("Login","",128));assert(shows==0);
 std::cout<<"Software keyboard real helper: release-before-launch, success, private input, creation/show errors and non-confirmed close PASS\n";
}
'''
with tempfile.TemporaryDirectory() as temporary:
    path = Path(temporary)
    (path / 'test.cpp').write_text(stubs + method + cases)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(path/'test.cpp'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
