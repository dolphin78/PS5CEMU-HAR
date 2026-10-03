// SPDX-License-Identifier: GPL-3.0-or-later
// Host harness for the portable streaming server and QR encoder.
#include "gamepad.h"
#include "qr.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>
#include <thread>
int main(int argc,char** argv){
 if(argc==3 && std::string(argv[1])=="--qr"){
  auto bytes=ps5stream::EncodeConnectionQR(argv[2]);
  std::cout.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());return 0;
 }
 const bool stress=argc==2 && std::string(argv[1])=="--stress";
 const int width=stress?1708:8,height=stress?960:4;
 std::vector<uint8_t> rgb(size_t(width)*height*3,128);
 if(stress){uint32_t random=1;for(auto& pixel:rgb){random=random*1664525+1013904223;pixel=random>>24;}}
 ps5stream::Configure(true);std::cout<<ps5stream::Status()<<std::endl;
 std::atomic<bool> alive{true};
 std::thread render([&]{while(alive){if(ps5stream::WantFrame())ps5stream::SubmitFrame(rgb,width,height);std::this_thread::sleep_for(std::chrono::milliseconds(10));}});
 std::string command;
 while(std::getline(std::cin,command)){
  if(command=="touch"){float x,y;bool down=ps5stream::Touch(x,y);std::cout<<down<<" "<<x<<" "<<y<<std::endl;}
  else if(command=="disable"){ps5stream::Configure(false);std::cout<<ps5stream::Status()<<std::endl;}
  else if(command=="enable"){ps5stream::Configure(true);std::cout<<ps5stream::Status()<<std::endl;}
  else break;
 }
 alive=false;render.join();ps5stream::Configure(false);
}
