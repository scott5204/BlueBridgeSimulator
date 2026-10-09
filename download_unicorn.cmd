chcp 65001 >nul
cd /d "%~dp0"
curl.exe -sL --retry 3 -o unicorn-2.1.4.tar.gz https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.4.tar.gz
curl.exe -sL --retry 3 -o unicorn-master.tar.gz https://github.com/unicorn-engine/unicorn/archive/refs/heads/master.tar.gz
