// SPDX-License-Identifier: GPL-3.0-or-later
#include "qr.h"
#include "../app/paths.h"
#include <cstdio>
#include <fstream>
namespace ps5stream {
std::string ConnectionQR(const std::string& url){
 static std::string previous,path;
 static unsigned generation=0;
 if(url==previous)return path;
 if(!path.empty())std::remove(path.c_str());
 previous=url;path.clear();
 const auto bytes=EncodeConnectionQR(url);if(bytes.empty())return path;
 // A new path avoids RmlUi reusing a cached image after the address changes.
 const std::string output=std::string(ps5paths::kRoot)+"/gamepad-qr-"+std::to_string(++generation)+".tga";
 std::ofstream file(output,std::ios::binary|std::ios::trunc);if(!file)return path;
 file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
 if(file)path=output;return path;
}
}
