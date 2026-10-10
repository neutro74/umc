# Turns a binary file into a C array header: cmake -DIN=x -DOUT=y -P embed.cmake
file(READ ${IN} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT} "#pragma once\nstatic const unsigned char kBridgeClass[] = {${bytes}};\n")
