#include "cyvr/player/file_browser.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace cyvr::player;
namespace fs=std::filesystem;
namespace {
int checks=0;
void require(bool valid,const char* message){++checks;if(!valid) throw std::runtime_error(message);}
template<class F> void rejects(F fn){
    bool rejected=false;
    try{fn();}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"Expected invalid input to be rejected");
}
struct Fixture {
    fs::path base=fs::canonical(fs::current_path());
    fs::path path=base/("cyvr-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture(){fs::create_directories(path/"root/folder");fs::create_directories(path/"outside");}
    ~Fixture(){
        // Validate the final absolute target before recursively removing our
        // own test data. Never follow a computed path outside the test folder.
        std::error_code error;
        const auto actual=fs::canonical(path,error);
        if(!error && actual.parent_path()==base && actual.filename().u8string().rfind("cyvr-test-",0)==0)
            fs::remove_all(actual,error);
    }
    void file(const char* name){std::ofstream(path/"root"/name,std::ios::binary)<<"fixture";}
};
void source_tests(){
    require(MediaSource::network("HTTPS://host.example/video?token=secret").location.rfind("https:",0)==0,"Normalize URL scheme");
    const auto source=MediaSource::network("https://user:password@host.example/private-token/video.m3u8?signature=secret#data");
    require(source.display_name()=="https://host.example","Display name must omit credentials, paths and query tokens");
    require(MediaSource::network("udp://239.0.0.1:1234").kind==SourceKind::network,"Multicast source type");
    require(MediaSource::descriptor(3,10,20).length==20,"Descriptor slices preserve lengths");
    rejects([]{MediaSource::network("file:///secret.mp4");});
    rejects([]{MediaSource::network("https://");});
    rejects([]{MediaSource::network("https://host/a\nInjected: value");});
    rejects([]{MediaSource::file("content://provider/document/42");});
    rejects([]{MediaSource::file("");});
    rejects([]{MediaSource::descriptor(-1);});
    rejects([]{MediaSource::descriptor(3,-1);});
    rejects([]{MediaSource::descriptor(3,0,-2);});
    rejects([]{MediaSource::descriptor(3,std::numeric_limits<std::int64_t>::max(),1);});
    StreamOptions options;options.validate();
    options.read_timeout_ms=0;rejects([&]{options.validate();});
    options=StreamOptions{};options.probe_bytes=100000000;rejects([&]{options.validate();});
    Cancellation cancel;require(!cancel.cancelled(),"Cancellation starts inactive");
    cancel.cancel();require(cancel.cancelled(),"Cancellation can be triggered");
}
void browser_tests(){
    Fixture fixture;
    for(const char* name:{"Movie1.MP4","audio.FLAC","scene.spz","stream.m3u8","notes.txt",".hidden.mp4"}) fixture.file(name);
    FileBrowser browser(fixture.path/"root");
    const auto listing=browser.list();
    require(listing.entries.size()==5,"Default browser filters hidden and unrelated files");
    require(listing.entries[0].kind==FileKind::directory,"Directories sort before files");
    require(listing.entries[1].name=="audio.FLAC","File sorting is case insensitive");
    require(!listing.truncated && !listing.cancelled,"Complete scans report completion");
    BrowseOptions options;options.search="mov";
    require(browser.list({},options).entries.size()==1,"Case insensitive search");
    options=BrowseOptions{};options.show_hidden=options.show_other=true;
    require(browser.list({},options).entries.size()==7,"Optional hidden/unrelated files");
    options=BrowseOptions{};options.max_entries=2;
    const auto limited=browser.list({},options);
    require(limited.entries.size()==2 && limited.truncated,"Large folders have bounded snapshots");
    options=BrowseOptions{};options.scan_budget=1;
    require(browser.list({},options).truncated,"Scan budget is enforced independently of filter matches");
    Cancellation cancel;cancel.cancel();
    const auto cancelled=browser.list({},BrowseOptions{},&cancel);
    require(cancelled.cancelled && cancelled.entries.empty(),"Browser work can be cancelled");
    const auto opened=browser.open_file("Movie1.MP4");
    require(opened.kind==SourceKind::file && fs::u8path(opened.location).is_absolute(),"Selections yield absolute media paths");
    rejects([&]{browser.list("..");});
    rejects([&]{browser.open_file("folder");});
    rejects([&]{browser.list(fixture.path);});
    rejects([&]{BrowseOptions invalid;invalid.max_entries=0;browser.list({},invalid);});
    require(classify_extension("scene.PLY")==FileKind::splat,"Splat extension hint");
    require(classify_extension("unknown.blob")==FileKind::other,"Unknown extensions remain explicit");
    std::error_code error;
    fs::create_directory_symlink(fixture.path/"outside",fixture.path/"root/escape",error);
    if(!error){
        rejects([&]{browser.list("escape");});
        const auto after_link=browser.list();
        require(after_link.entries.size()==5,"Links outside root are not exposed");
    }
}
}
int main(){
    try{source_tests();browser_tests();std::cout<<checks<<" I/O checks passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
