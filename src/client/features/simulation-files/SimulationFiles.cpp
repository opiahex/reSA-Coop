// Read-only background compatibility diagnostics. File contents never leave disk.
#include <windows.h>
#include <atomic>
#include <future>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <vector>
#include <map>
#include <array>
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/config/Config.h"
namespace coop {namespace {
struct Digest{std::string name;uint8_t status=0;uint32_t crc=0;uint64_t size=0;};
using List=std::vector<Digest>;
std::atomic<bool> cancel=false;
Digest Hash(const std::filesystem::path&root,const std::string&name){
 Digest d;d.name=name;std::error_code ec;auto p=root/std::filesystem::path(Utf8ToWide(name));
 if(!std::filesystem::exists(p,ec))return d;
 auto stamp=std::filesystem::last_write_time(p,ec);std::ifstream f(p,std::ios::binary);if(!f){d.status=2;return d;}
 std::array<char,65536>b;d.status=1;
 while(f&&!cancel){f.read(b.data(),b.size());auto n=f.gcount();if(n>0){d.size+=n;d.crc=Crc32(b.data(),(size_t)n,d.crc);}Sleep(1);}
 if(cancel||(!f.eof()&&f.fail())||std::filesystem::last_write_time(p,ec)!=stamp)d.status=2;
 return d;
}
Digest ImageCollision(const std::filesystem::path&root,const std::string&name){
 Digest d;d.name=name+" (*.col)";std::ifstream f(root/std::filesystem::path(Utf8ToWide(name)),std::ios::binary);if(!f)return d;
 uint32_t magic=0,count=0;f.read((char*)&magic,4);f.read((char*)&count,4);
 if(magic!=0x32524556||count>100000){d.status=2;return d;}
 struct Entry{uint32_t sector;uint16_t streaming,archive;char name[24];};std::vector<Entry> entries(count);
 f.read((char*)entries.data(),(std::streamsize)entries.size()*sizeof(Entry));if(!f){d.status=2;return d;}
 d.status=1;std::array<char,65536>b;
 for(auto&e:entries){if(cancel){d.status=2;break;}std::string n(e.name,strnlen(e.name,24));n=ToLower(n);if(n.size()<4||n.substr(n.size()-4)!=".col")continue;
   uint32_t bytes=(uint32_t)(e.archive?e.archive:e.streaming)*2048u;if(!bytes||bytes>32*1024*1024){d.status=2;break;}
   d.crc=Crc32(n.data(),n.size(),d.crc);f.seekg((uint64_t)e.sector*2048);
   while(bytes&&f&&!cancel){size_t size=std::min<size_t>(bytes,b.size());f.read(b.data(),size);if(!f){d.status=2;break;}d.crc=Crc32(b.data(),size,d.crc);d.size+=size;bytes-=(uint32_t)size;Sleep(1);}
 }
 return d;
}
class SimulationFiles:public Module{
 std::future<List> worker;List files;std::map<uint8_t,List> remote;std::map<uint8_t,uint32_t> warned;bool started=false,send=true;uint64_t last=0;
 const char*Name()const override{return "SimulationFiles";}
 void Compare(uint8_t from){
  if(files.empty()||!remote.count(from))return;int bad=0;uint32_t fingerprint=0;
  for(auto&a:files){auto&list=remote[from];auto i=std::find_if(list.begin(),list.end(),[&](auto&b){return a.name==b.name;});
   if(i==list.end()||a.status!=i->status||(a.status==1&&(a.crc!=i->crc||a.size!=i->size))){++bad;fingerprint=Crc32(a.name.data(),a.name.size(),fingerprint);if(i!=list.end())fingerprint=Crc32(&i->crc,sizeof(i->crc),fingerprint);LOGW("Simulation file differs: peer=%u file=%s local=%u/%llu/%08X remote=%u/%llu/%08X",from,a.name.c_str(),a.status,(unsigned long long)a.size,a.crc,i==list.end()?255:i->status,i==list.end()?0:(unsigned long long)i->size,i==list.end()?0:i->crc);}}
  for(auto&b:remote[from])if(std::none_of(files.begin(),files.end(),[&](auto&a){return a.name==b.name;})){++bad;fingerprint=Crc32(b.name.data(),b.name.size(),fingerprint);LOGW("Simulation file only on peer %u: %s",from,b.name.c_str());}
  LOGI("Simulation file comparison: peer=%u entries=%u differences=%d",from,(unsigned)files.size(),bad);
  if(bad&&(!warned.count(from)||warned[from]!=fingerprint)){warned[from]=fingerprint;ui::TheChat().AddSystem(Format(TR("Игровые файлы отличаются у %s (%d). Подробности: coop.log.","Simulation files differ with %s (%d). See coop.log."),Net().PeerName(from).c_str(),bad));}
  if(!bad)warned.erase(from);
 }
 void OnInstallHooks()override{
  Net().SetHandler(proto::G_SIM_FILES,[](uint8_t from,Reader&r){auto&self=Instance();List list;int count=r.U8();if(count>40)return;
   for(int n=0;n<count;++n){Digest d;d.name=r.Str(120);d.status=r.U8();uint32_t lo=r.U32(),hi=r.U32();d.size=lo|((uint64_t)hi<<32);d.crc=r.U32();if(d.status>2)return;list.push_back(d);}
   if(!r.Ok()||from>=3)return;self.remote[from]=std::move(list);self.Compare(from);
  });
 }
 void OnPostProcess()override{
  if(!Net().Online())return;
  if(!started){started=true;cancel=false;auto root=std::filesystem::path(ModuleDir(nullptr));
   worker=std::async(std::launch::async,[root]{SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);List out;
    for(const char*n:{"gta_sa.exe","data/script/main.scm","data/script/script.img","data/handling.cfg","data/weapon.dat","data/vehicles.ide","data/peds.ide","data/pedstats.dat","data/surface.dat","data/surfinfo.dat","data/default.dat","data/gta.dat"}){if(cancel)break;out.push_back(Hash(root,n));}
    std::error_code ec;std::vector<std::string> cols;for(std::filesystem::recursive_directory_iterator i(root/"models/coll",ec),end;!ec&&i!=end;i.increment(ec))if(i->is_regular_file(ec)&&ToLower(i->path().extension().string())==".col")cols.push_back(i->path().lexically_relative(root).generic_string());
    std::sort(cols.begin(),cols.end());for(auto&n:cols)if(!cancel&&out.size()<37)out.push_back(Hash(root,n));
    if(!cancel)out.push_back(ImageCollision(root,"models/gta3.img"));if(!cancel)out.push_back(ImageCollision(root,"models/gta_int.img"));return out;
   });
  }
  if(worker.valid()&&worker.wait_for(std::chrono::seconds(0))==std::future_status::ready){try{files=worker.get();}catch(const std::exception&e){LOGW("Simulation file scan failed: %s",e.what());files.clear();}catch(...){LOGW("Simulation file scan failed");files.clear();}
   for(auto&d:files)LOGI("Simulation digest: %s status=%u bytes=%llu crc32=%08X",d.name.c_str(),d.status,(unsigned long long)d.size,d.crc);
   for(auto&[peer,list]:remote)Compare(peer);send=true;
  }
  if(!files.empty()&&(send||NowMs()-last>30000)){
   Writer w(proto::G_SIM_FILES);w.U8((uint8_t)files.size());for(auto&d:files){w.Str(d.name,120);w.U8(d.status);w.U32((uint32_t)d.size);w.U32((uint32_t)(d.size>>32));w.U32(d.crc);}Net().Send(proto::CH_RELIABLE,w);last=NowMs();send=false;
  }
 }
 void OnPeerJoin(uint8_t)override{send=true;}
 void OnPeerLeave(uint8_t id)override{remote.erase(id);warned.erase(id);}
 void OnSessionEnd()override{remote.clear();warned.clear();send=true;}
public:static SimulationFiles&Instance(){static auto*s=new SimulationFiles;return *s;}
};
struct Reg{Reg(){core::RegisterModule(&SimulationFiles::Instance());}}reg;
}}
