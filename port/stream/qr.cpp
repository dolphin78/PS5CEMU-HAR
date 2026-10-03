// SPDX-License-Identifier: GPL-3.0-or-later
// Fixed version 2-L, byte-mode QR for the local IPv4 connection URL (up to 32 bytes).
#include "qr.h"
#include <array>
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <cstdint>
namespace ps5stream {
namespace {
uint8_t Multiply(uint8_t a,uint8_t b){uint8_t result=0;for(int i=0;i<8;i++){if(b&1)result^=a;bool high=a&128;a<<=1;if(high)a^=0x1d;b>>=1;}return result;}
}
std::vector<uint8_t> EncodeConnectionQR(const std::string& url){
 if(url.rfind("http://",0)!=0||url.size()>32)return {};
 std::vector<bool> bits;
 auto append=[&](unsigned value,int count){for(int i=count-1;i>=0;i--)bits.push_back((value>>i)&1);};
 append(4,4);append(url.size(),8);for(unsigned char c:url)append(c,8);
 for(int i=0;i<4 && bits.size()<272;i++)bits.push_back(false);
 while(bits.size()%8)bits.push_back(false);
 std::vector<uint8_t> data;
 for(size_t i=0;i<bits.size();i+=8){uint8_t b=0;for(int j=0;j<8;j++)b=(b<<1)|bits[i+j];data.push_back(b);}
 for(int i=0;data.size()<34;i++)data.push_back(i%2?0x11:0xec);
 // Reed-Solomon: version 2-L has one block, 34 data and 10 parity codewords.
 std::array<uint8_t,10> divisor{},parity{};divisor[9]=1;uint8_t root=1;
 for(int i=0;i<10;i++){for(int j=0;j<10;j++){divisor[j]=Multiply(divisor[j],root);if(j<9)divisor[j]^=divisor[j+1];}root=Multiply(root,2);}
 for(uint8_t b:data){uint8_t factor=b^parity[0];for(int j=0;j<9;j++)parity[j]=parity[j+1];parity[9]=0;for(int j=0;j<10;j++)parity[j]^=Multiply(divisor[j],factor);}
 data.insert(data.end(),parity.begin(),parity.end());
 constexpr int n=25;bool modules[n][n]{},fixed[n][n]{};
 auto set=[&](int x,int y,bool black){if(x>=0&&y>=0&&x<n&&y<n){modules[y][x]=black;fixed[y][x]=true;}};
 for(int i=0;i<n;i++){set(6,i,i%2==0);set(i,6,i%2==0);}
 auto finder=[&](int cx,int cy){for(int y=-4;y<=4;y++)for(int x=-4;x<=4;x++){int d=std::max(std::abs(x),std::abs(y));set(cx+x,cy+y,d!=2&&d!=4);}};
 finder(3,3);finder(n-4,3);finder(3,n-4);
 for(int y=-2;y<=2;y++)for(int x=-2;x<=2;x++)set(18+x,18+y,std::max(std::abs(x),std::abs(y))!=1);
 // Format: L error correction (01), mask 0, BCH and XOR mask.
 int format=8,rem=format;for(int i=0;i<10;i++)rem=(rem<<1)^((rem>>9)*0x537);format=((format<<10)|rem)^0x5412;
 auto f=[&](int i){return bool((format>>i)&1);};
 for(int i=0;i<=5;i++)set(8,i,f(i));set(8,7,f(6));set(8,8,f(7));set(7,8,f(8));for(int i=9;i<15;i++)set(14-i,8,f(i));
 for(int i=0;i<8;i++)set(n-1-i,8,f(i));for(int i=8;i<15;i++)set(8,n-15+i,f(i));set(8,n-8,true);
 int index=0;
 for(int right=n-1;right>=1;right-=2){if(right==6)right=5;for(int vertical=0;vertical<n;vertical++){int y=((right+1)&2)==0?n-1-vertical:vertical;for(int j=0;j<2;j++){int x=right-j;if(fixed[y][x])continue;bool b=index<352?((data[index/8]>>(7-index%8))&1):false;modules[y][x]=b^((x+y)%2==0);index++;}}}
 // Uncompressed 32-bit top-origin TGA; four-module quiet zone.
 constexpr int scale=6,side=(n+8)*scale;
 std::array<unsigned char,18> header{};header[2]=2;header[12]=side&255;header[13]=side>>8;header[14]=side&255;header[15]=side>>8;header[16]=32;header[17]=0x28;
 std::vector<uint8_t> result(header.begin(),header.end());
 for(int y=0;y<side;y++)for(int x=0;x<side;x++){int mx=x/scale-4,my=y/scale-4;uint8_t color=(mx>=0&&my>=0&&mx<n&&my<n&&modules[my][mx])?0:255;for(int c=0;c<3;c++)result.push_back(color);result.push_back(255);}
 return result;
}
}
