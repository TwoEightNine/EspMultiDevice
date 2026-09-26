# Lua 5.4.9 in ESP Media

The files in this directory are unmodified Lua 5.4.9 source from the official `lua-5.4.9.tar.gz` release at <https://www.lua.org/ftp/> (SHA-256 `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`). Lua is distributed under the MIT license; its copyright and license notice is included in `lua.h` and applies to the source files here. `README.upstream` is the release README.

Arduino builds the selected Lua C source through `src/vendor_shims/lua/`. The standalone interpreter/compiler and file, OS, debug, package, coroutine, and all-libraries initialization units are deliberately excluded. `AppRuntime` opens only selected libraries and removes dynamic code-loading functions from the base library. Keep upstream source unmodified; adapt integration in project code and re-check the OTA-slot size when changing Lua or its built-in libraries.
