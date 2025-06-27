#!/bin/sh
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

V_SCRIPT_PATH=$(dirname "${0}")
cd "${V_SCRIPT_PATH}/../Resources/"

V_KERNEL_RELEASE=$(uname -r | cut -d. -f1)
if [[ "${V_KERNEL_RELEASE}" -ge 11 ]]; then
    K_LIBRARY_PATH=DYLD_LIBRARY_PATH
    K_FRAMEWORK_PATH=DYLD_FRAMEWORK_PATH
else
    K_LIBRARY_PATH=DYLD_FALLBACK_LIBRARY_PATH
    K_FRAMEWORK_PATH=DYLD_FALLBACK_FRAMEWORK_PATH
fi

V_OS_VERSION=$(sw_vers -productVersion | cut -d. -f1)
if [[ "${V_OS_VERSION}" -lt 11 ]] && [[ ! -f "libz_patched.txt" ]]; then
    cp "/usr/lib/libz.1.dylib" "libz.1.3.1.dylib"
    echo "This file signifies (to the launch script) that libz.1.3.1.dylib has been patched by replacing it with /usr/lib/libz.1.dylib. This happened because the included libz is unreadable on Mac OS versions below 10.15. The game will function normally." > "libz_patched.txt"
fi

if [[ -f cataclysm-tlg ]]; then
    V_SHELL_SCRIPT="export PATH=${PATH} ${K_LIBRARY_PATH}=. ${K_FRAMEWORK_PATH}=.; cd '${PWD}' && ./cataclysm-tlg; exit"
    osascript -e "tell application \"Terminal\" to activate do script \"${V_SHELL_SCRIPT}\""
else
    export ${K_LIBRARY_PATH}=. ${K_FRAMEWORK_PATH}=.
    ./cataclysm-tlg-tiles
fi
