"""Optional thread-start scheduling observer for the pinned RTC dependencies.

Naming is already performed once at each thread's entry. Hook those helpers,
not packet callbacks. libdatachannel DTLS work uses the observed shared pool;
the configured USE_NICE=OFF backend uses libjuice's observed UDP thread.
"""
from pathlib import Path
import shutil


def apply(root):
    rtc = root / 'third_party/rtc/libdatachannel'
    header = root / 'tools/rtc/thread-observation.h'

    def replace(path, old, new):
        source = path.read_text()
        source = source.replace(new, old)
        if source.count(old) != 1:
            raise SystemExit('Unrecognized pinned thread observation site: ' + str(path))
        path.write_text(source.replace(old, new))

    sites = (
        (rtc / 'src/impl/utils.cpp', '#include "utils.hpp"',
         'void thread_set_name_self(const char *name) {'),
        (rtc / 'deps/libjuice/src/thread.h', '#define JUICE_THREAD_H',
         'static inline void thread_set_name_self(const char *name) {'),
        (rtc / 'deps/usrsctp/usrsctplib/netinet/sctp_userspace.c',
         '#include <netinet/sctp_os_userspace.h>',
         'sctp_userspace_set_threadname(const char *name)\n{'),
    )
    for path, include, entry in sites:
        shutil.copyfile(header, path.parent / 'fogthreadobservation.h')
        replace(path, include, include + '\n#include "fogthreadobservation.h"')
        replace(path, entry, entry + '\n\tfog_observe_thread_start(name);')


if __name__ == '__main__':
    apply(Path(__file__).resolve().parents[2])
