#include "envelope.h"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace scrp {
namespace {
void require(bool ok, const char* error) {
    if (!ok) throw std::invalid_argument(error);
}
std::array<unsigned char, 16> random_bytes() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("SCRP random generator failed");
    return bytes;
}
bool uuid(const std::string& s) {
    if (s.size() != 36 || s[14] != '4' || std::string("89ab").find(s[19]) == std::string::npos) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (std::string("0123456789abcdef").find(s[i]) == std::string::npos) return false;
    }
    return true;
}
void nullable_string(const Json::Value& v) {
    require(v.isNull() || (v.isString() && !v.asString().empty()), "Invalid nullable SCRP string");
}
void validate_tree(const Json::Value& v,unsigned int depth=1) {
    require(depth<=16,"JSON nesting exceeds limit");
    if(v.isString()) require(valid_utf8(v.asString()),"Invalid decoded JSON UTF-8");
    if(v.type()==Json::realValue) require(std::isfinite(v.asDouble()),"Non-finite JSON number");
    if(v.isObject()) for(const auto& key:v.getMemberNames()) {
        require(valid_utf8(key),"Invalid decoded JSON key"); validate_tree(v[key],depth+1);
    }
    else if(v.isArray()) for(const auto& item:v) validate_tree(item,depth+1);
}
// Host sender emits microseconds; accept its RFC3339 UTC 1..6 digit contract.
ULONGLONG utc_ticks(const std::string& s) {
    require(s.size() == 20 || (s.size() >= 22 && s.size() <= 27), "Invalid SCRP timestamp");
    require(s[4]=='-' && s[7]=='-' && s[10]=='T' && s[13]==':' && s[16]==':' && s.back()=='Z', "Invalid SCRP timestamp");
    auto number = [&](std::size_t p, std::size_t n) {
        WORD v = 0;
        for (std::size_t i=p; i<p+n; ++i) { require(s[i]>='0' && s[i]<='9', "Invalid SCRP timestamp"); v=WORD(v*10+s[i]-'0'); }
        return v;
    };
    SYSTEMTIME t{};
    t.wYear=number(0,4); t.wMonth=number(5,2); t.wDay=number(8,2);
    t.wHour=number(11,2); t.wMinute=number(14,2); t.wSecond=number(17,2);
    ULONGLONG fraction = 0;
    if (s.size() != 20) {
        require(s[19]=='.', "Invalid SCRP timestamp");
        for (std::size_t i=20; i<s.size()-1; ++i) {
            require(s[i]>='0' && s[i]<='9', "Invalid SCRP timestamp");
            fraction = fraction*10 + s[i]-'0';
        }
        for (std::size_t digits=s.size()-21; digits<7; ++digits) fraction *= 10;
    }
    FILETIME ft{};
    require(SystemTimeToFileTime(&t, &ft), "Invalid SCRP date");
    SYSTEMTIME round{}; FileTimeToSystemTime(&ft, &round);
    require(round.wYear==t.wYear && round.wMonth==t.wMonth && round.wDay==t.wDay,
            "Invalid SCRP calendar date");
    return ((ULONGLONG(ft.dwHighDateTime)<<32) | ft.dwLowDateTime) + fraction;
}
void validate_shape(const Envelope& e) {
    validate_session(e.session);
    require(e.version=="1.0" && e.sequence_number>0 && uuid(e.message_id), "Invalid SCRP version, sequence or message ID");
    nullable_string(e.connection_id); nullable_string(e.task_id); nullable_string(e.action_id);
    nullable_string(e.correlation_id); nullable_string(e.status);
    require(e.error.isNull() || e.error.isObject(), "Invalid SCRP error");
    require(e.payload.isObject() && !e.type.empty(), "Invalid SCRP payload or type");
    require(e.nonce.size()==22 && std::string("AQgw").find(e.nonce.back())!=std::string::npos,
            "Invalid SCRP nonce");
    for (char c:e.nonce) require(std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_").find(c)!=std::string::npos, "Invalid SCRP nonce");
    utc_ticks(e.timestamp);
}
}
void validate_session(const SessionContext& s) {
    require(!s.session_id.empty() && !s.runtime_id.empty() && s.generation>0 &&
            valid_utf8(s.session_id) && valid_utf8(s.runtime_id), "Invalid Host session context");
}
bool valid_uuid_v4(const std::string& value) { return uuid(value); }
void validate_utc_timestamp(const std::string& value) { utc_ticks(value); }
bool valid_utf8(const std::string& s) {
    return s.empty() || (s.size()<=static_cast<std::size_t>(INT_MAX) &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0)>0);
}
std::string uuid_v4() {
    auto b=random_bytes(); b[6]=(b[6]&15)|64; b[8]=(b[8]&63)|128;
    std::string result; const char* hex="0123456789abcdef";
    for (std::size_t i=0;i<b.size();++i) {
        if(i==4||i==6||i==8||i==10) result+='-';
        result+=hex[b[i]>>4]; result+=hex[b[i]&15];
    }
    return result;
}
std::string random_nonce() {
    auto b=random_bytes(); const char* chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out; unsigned int bits=0; int count=0;
    for(auto c:b) { bits=(bits<<8)|c; count+=8; while(count>=6) { count-=6; out+=chars[(bits>>count)&63]; } }
    if(count) out+=chars[(bits<<(6-count))&63];
    return out;
}
std::string utc_now() {
    SYSTEMTIME t{}; GetSystemTime(&t); char s[32];
    std::snprintf(s,sizeof(s),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    return s;
}
std::string json_text(const Json::Value& v) {
    validate_tree(v);
    Json::StreamWriterBuilder b; b["indentation"]=""; b["emitUTF8"]=true;
    const auto result=Json::writeString(b,v);
    require(valid_utf8(result), "Invalid JSON UTF-8");
    return result;
}
Json::Value parse_json(const std::string& text, std::size_t max_bytes) {
    require(text.size()<=max_bytes && valid_utf8(text), "JSON exceeds limit or has invalid UTF-8");
    Json::CharReaderBuilder b;
    Json::CharReaderBuilder::strictMode(&b.settings_);
    b["stackLimit"]=16; b["rejectDupKeys"]=true; b["failIfExtra"]=true;
    b["allowSpecialFloats"]=false; b["skipBom"]=false;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    Json::Value v; std::string errors;
    require(reader->parse(text.data(), text.data()+text.size(), &v, &errors) && v.isObject(), "Invalid SCRP JSON object");
    validate_tree(v);
    return v;
}
std::string serialize(const Envelope& e) {
    validate_shape(e);
    Json::Value v(Json::objectValue);
    v["version"]=e.version; v["session_id"]=e.session.session_id; v["runtime_id"]=e.session.runtime_id;
    v["generation"]=Json::UInt64(e.session.generation); v["connection_id"]=e.connection_id;
    v["message_id"]=e.message_id; v["task_id"]=e.task_id; v["action_id"]=e.action_id;
    v["sequence_number"]=Json::UInt64(e.sequence_number); v["timestamp"]=e.timestamp;
    v["nonce"]=e.nonce; v["type"]=e.type; v["correlation_id"]=e.correlation_id;
    v["status"]=e.status; v["error"]=e.error; v["payload"]=e.payload;
    return json_text(v);
}
Envelope parse_envelope(const std::string& text, std::size_t limit) {
    auto v=parse_json(text,limit);
    const std::set<std::string> keys={"version","session_id","runtime_id","generation","connection_id","message_id","task_id","action_id","sequence_number","timestamp","nonce","type","correlation_id","status","error","payload"};
    require(v.size()==keys.size(), "Missing or unknown SCRP field");
    for(const auto& k:v.getMemberNames()) require(keys.count(k)!=0, "Unknown SCRP field");
    for(const auto* k:{"version","session_id","runtime_id","message_id","timestamp","nonce","type"}) require(v[k].isString(), "SCRP field must be a string");
    require(v["generation"].isUInt64() && v["sequence_number"].isUInt64(), "SCRP counters must be unsigned integers");
    // JsonCpp considers integral real numbers convertible; require JSON integer representation.
    require(v["generation"].type()!=Json::realValue && v["sequence_number"].type()!=Json::realValue, "SCRP counters must be JSON integers");
    Envelope e;
    e.version=v["version"].asString(); e.session={v["session_id"].asString(),v["runtime_id"].asString(),v["generation"].asUInt64()};
    e.connection_id=v["connection_id"]; e.message_id=v["message_id"].asString(); e.task_id=v["task_id"]; e.action_id=v["action_id"];
    e.sequence_number=v["sequence_number"].asUInt64(); e.timestamp=v["timestamp"].asString(); e.nonce=v["nonce"].asString();
    e.type=v["type"].asString(); e.correlation_id=v["correlation_id"]; e.status=v["status"]; e.error=v["error"]; e.payload=v["payload"];
    validate_shape(e); return e;
}
void validate_incoming(const Envelope& e,const SessionContext& s,std::uint64_t sequence) {
    require(e.session.session_id==s.session_id && e.session.runtime_id==s.runtime_id && e.session.generation==s.generation,
            "SCRP session mismatch");
    require(e.sequence_number==sequence, "SCRP sequence mismatch");
    const auto a=utc_ticks(e.timestamp), b=utc_ticks(utc_now());
    require((a>b?a-b:b-a)<=600000000ULL, "SCRP timestamp outside 60 second window");
}
Envelope make_envelope(const SessionContext& s,const Json::Value& c,std::uint64_t seq,const std::string& type,const Json::Value& payload) {
    Envelope e; e.session=s; e.connection_id=c; e.sequence_number=seq; e.type=type; e.payload=payload;
    e.message_id=uuid_v4(); e.nonce=random_nonce(); e.timestamp=utc_now(); return e;
}
} // namespace scrp
