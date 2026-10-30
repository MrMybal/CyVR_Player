#include "cyvr/player/clearkey.hpp"
#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>
#include <filesystem>
#ifdef _WIN32
#include <io.h>
#else
#include <poll.h>
#include <unistd.h>
#endif
namespace cyvr::player {
namespace {
[[noreturn]] void invalid(){throw std::invalid_argument("Invalid ClearKey JWK: expected at most 16 distinct AES-128 keys with 16-byte KIDs");}
struct Json {
    enum Kind {object,array,string,scalar} kind=scalar;
    std::string text;std::map<std::string,Json> fields;std::vector<Json> items;
};
class Reader {
    std::string_view input;std::size_t at{},nodes{};
    char current(){while(at<input.size() && (input[at]==' ' || input[at]=='\r' || input[at]=='\n' || input[at]=='\t'))++at;return at<input.size()?input[at]:'\0';}
    void expect(char c){if(current()!=c)invalid();++at;}
    std::string text(){
        expect('"');std::string out;
        while(at<input.size()){
            unsigned char c=input[at++];if(c=='"')return out;if(c<32)invalid();
            if(c=='\\'){
                if(at==input.size())invalid();c=input[at++];
                if(c=='u'){
                    unsigned code=0;for(int i=0;i<4;++i){if(at==input.size())invalid();const char h=input[at++];const int n=h>='0'&&h<='9'?h-'0':h>='a'&&h<='f'?h-'a'+10:h>='A'&&h<='F'?h-'A'+10:-1;if(n<0)invalid();code=code*16+n;}
                    if(code<32 || code>126)invalid();c=static_cast<unsigned char>(code);
                }else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='b')c='\b';else if(c=='f')c='\f';else if(c!='"' && c!='\\' && c!='/')invalid();
            }
            out+=char(c);if(out.size()>4096)invalid();
        }
        invalid();
    }
    Json value(int depth){
        if(depth>8 || ++nodes>1024)invalid();Json out;const char c=current();
        if(c=='"'){out.kind=Json::string;out.text=text();return out;}
        if(c=='{' || c=='['){
            ++at;out.kind=c=='{'?Json::object:Json::array;const char end=c=='{'?'}':']';
            if(current()==end){++at;return out;}
            for(;;){if(c=='{'){auto name=text();expect(':');if(!out.fields.emplace(std::move(name),value(depth+1)).second)invalid();}else out.items.push_back(value(depth+1));if(current()==end){++at;return out;}expect(',');}
        }
        // Additional JWK metadata can contain JSON booleans/null, but never
        // arbitrary unquoted text or numbers that could hide malformed keys.
        for(auto literal:{std::string_view("true"),std::string_view("false"),std::string_view("null")})if(input.substr(at,literal.size())==literal){at+=literal.size();return out;}
        invalid();
    }
public:
    explicit Reader(std::string_view s):input(s){if(s.empty() || s.size()>65536)invalid();}
    Json read(){auto out=value(0);if(current())invalid();return out;}
};
const Json& field(const Json& object,const char* name){auto i=object.fields.find(name);if(object.kind!=Json::object || i==object.fields.end())invalid();return i->second;}
std::array<std::uint8_t,16> bytes(const Json& value){
    if(value.kind!=Json::string || (value.text.size()!=22 && value.text.size()!=24))invalid();
    if(value.text.size()==24 && value.text.substr(22)!="==")invalid();
    std::array<std::uint8_t,16> out{};unsigned accumulator=0,bits=0,index=0;
    for(std::size_t n=0;n<22;++n){const char c=value.text[n];const int v=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='-'?62:c=='_'?63:-1;if(v<0)invalid();accumulator=(accumulator<<6)|unsigned(v);bits+=6;if(bits>=8){bits-=8;if(index>=out.size())invalid();out[index++]=std::uint8_t(accumulator>>bits);}}
    if(index!=16 || (accumulator&15))invalid();return out;
}
}
std::vector<ContentKey> parse_clear_keys(std::string_view text){
    const auto json=Reader(text).read();const auto& keys=field(json,"keys");
    if(keys.kind!=Json::array || keys.items.empty() || keys.items.size()>16)invalid();
    std::vector<ContentKey> result;
    for(const auto& item:keys.items){const auto& type=field(item,"kty");if(type.kind!=Json::string || type.text!="oct")invalid();ContentKey key{bytes(field(item,"kid")),bytes(field(item,"k"))};if(std::any_of(result.begin(),result.end(),[&](const auto& x){return x.id==key.id;}))invalid();result.push_back(key);}
    return result;
}
std::vector<ContentKey> read_clear_keys(const MediaSource& source,const Cancellation& cancel){
    if(source.kind==SourceKind::network)throw std::invalid_argument("ClearKey files must be local or explicitly selected Android documents");
    std::string data;data.reserve(4096);
    if(source.kind==SourceKind::file){
        std::ifstream file(std::filesystem::u8path(source.location),std::ios::binary);if(!file)throw std::runtime_error("Cannot open ClearKey file");
        char block[4096];while(file){if(cancel.cancelled())throw std::runtime_error("ClearKey loading cancelled");file.read(block,sizeof(block));data.append(block,std::size_t(file.gcount()));if(data.size()>65536)invalid();}
    }else{
        if(source.length>65536)invalid();std::int64_t position=0;
        for(;;){if(cancel.cancelled())throw std::runtime_error("ClearKey loading cancelled");char block[4096];int requested=sizeof(block);if(source.length>=0)requested=int(std::min<std::int64_t>(requested,source.length-position));if(!requested)break;
#ifdef _WIN32
            if(_lseeki64(source.fd,source.offset+position,SEEK_SET)<0)throw std::runtime_error("ClearKey document must be seekable");
            const int count=_read(source.fd,block,requested);
#else
            const auto count=pread(source.fd,block,requested,source.offset+position);
#endif
            if(count<0)throw std::runtime_error("Cannot read ClearKey document");if(!count)break;position+=count;data.append(block,std::size_t(count));if(data.size()>65536)invalid();
        }
    }
    return parse_clear_keys(data);
}
}
