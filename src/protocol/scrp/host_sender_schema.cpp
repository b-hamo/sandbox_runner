#include "host_sender_schema.h"
#include <algorithm>
#include <set>

namespace scrp {
namespace {
void check(bool ok) { if (!ok) throw std::invalid_argument("Host sender schema mismatch"); }
void fields(const Json::Value& v, std::initializer_list<const char*> names) {
    check(v.isObject() && v.size() == names.size());
    for (const auto name : names) check(v.isMember(name));
}
bool text(const Json::Value& v, std::size_t min, std::size_t max) {
    return v.isString() && v.asString().size() >= min && v.asString().size() <= max &&
           valid_utf8(v.asString()) && v.asString().find('\0') == std::string::npos;
}
bool number(const Json::Value& v, Json::UInt64 min, Json::UInt64 max) {
    return (v.type() == Json::intValue || v.type() == Json::uintValue) &&
           v.isUInt64() && v.asUInt64() >= min && v.asUInt64() <= max;
}
bool one(const Json::Value& v, std::initializer_list<const char*> values) {
    if (!v.isString()) return false;
    for (const auto value : values) if (v.asString() == value) return true;
    return false;
}
bool host_id(const Json::Value& v) {
    if (!text(v, 1, 64)) return false;
    const auto s = v.asString();
    const std::string alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    return alpha.find(s.front()) != std::string::npos &&
           s.find_first_not_of(alpha + "._:-") == std::string::npos;
}
void identity(const Envelope& e) {
    check(host_id(e.session.session_id) && host_id(e.session.runtime_id));
    for (const auto* v : {&e.connection_id, &e.task_id, &e.action_id}) check(v->isNull() || host_id(*v));
}
void date(const Json::Value& v) { check(v.isString()); validate_utc_timestamp(v.asString()); }
void upload(const Json::Value& v) {
    check(text(v, 16, 128) && v.asString().find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos);
}
void credential(const Json::Value& v) {
    fields(v, {"token", "expires_at"}); check(text(v["token"],32,512)); date(v["expires_at"]);
}
bool key(const Json::Value& v) {
    if (!v.isString()) return false;
    static const std::set<std::string> keys = {
        "enter","tab","escape","backspace","delete","insert","space","capslock","numlock","scrolllock","pause","printscreen","apps",
        "ctrl","alt","shift","win","hangul","hanja","up","down","left","right","home","end","pageup","pagedown",
        "f1","f2","f3","f4","f5","f6","f7","f8","f9","f10","f11","f12",
        "a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z",
        "0","1","2","3","4","5","6","7","8","9","minus","equal","leftbracket","rightbracket","backslash",
        "semicolon","quote","backtick","comma","period","slash","numpad0","numpad1","numpad2","numpad3","numpad4",
        "numpad5","numpad6","numpad7","numpad8","numpad9","numpadadd","numpadsubtract","numpadmultiply","numpaddivide","numpaddecimal","numpadenter"
    };
    return keys.count(v.asString()) != 0;
}
void action(const Json::Value& p) {
    fields(p, {"operation","arguments","observation_id","policy_version","timeout_ms"});
    check(text(p["observation_id"],1,64) && text(p["policy_version"],1,32) && number(p["timeout_ms"],100,60000));
    check(one(p["operation"], {"mouse.move","mouse.click","mouse.scroll","keyboard.type","keyboard.press","keyboard.hotkey","ui.click_element"}));
    const auto op = p["operation"].asString(); const auto& a = p["arguments"];
    if (op == "mouse.move" || op == "mouse.click" || op == "mouse.scroll") {
        if (op == "mouse.move") fields(a,{"x","y"});
        if (op == "mouse.click") { fields(a,{"x","y","button","click_count"}); check(one(a["button"],{"left","right","middle"}) && number(a["click_count"],1,2)); }
        if (op == "mouse.scroll") { fields(a,{"x","y","direction","steps"}); check(one(a["direction"],{"up","down","left","right"}) && number(a["steps"],1,10)); }
        check(number(a["x"],0,16383) && number(a["y"],0,16383));
    } else if (op == "keyboard.type") { fields(a,{"text"}); check(text(a["text"],1,4096)); }
    else if (op == "keyboard.press") { fields(a,{"key"}); check(key(a["key"])); }
    else if (op == "keyboard.hotkey") {
        fields(a,{"keys"}); const auto& keys = a["keys"];
        check(keys.isArray() && keys.size() >= 2 && keys.size() <= 4);
        std::set<std::string> seen;
        for (const auto& k : keys) check(key(k) && seen.insert(k.asString()).second);
    } else {
        fields(a,{"window_id","selector","button"});
        check((a["window_id"].isNull() || text(a["window_id"],1,64)) && one(a["button"],{"left","right","middle"}));
        fields(a["selector"],{"name","control_type"}); check(text(a["selector"]["name"],1,256));
        check(one(a["selector"]["control_type"],{"Button","Edit","CheckBox","RadioButton","ComboBox","ListItem","MenuItem","TabItem","Hyperlink","Text","TreeItem"}));
    }
}
void runtime(const Json::Value& p) {
    check(one(p["runtime_state"],{"PREPARING","READY","RUNNING","DEGRADED","FROZEN","UNRESPONSIVE","TERMINATED"}));
    check(p["worker_alive"].isBool() && number(p["queue_depth"],0,32));
}
}

Json::Value HostSenderSchema::hello(const SessionContext& s) const {
    check(host_id(s.session_id) && host_id(s.runtime_id));
    Json::Value p(Json::objectValue);
    p["supported_versions"] = Json::Value(Json::arrayValue); p["supported_versions"].append("1.0");
    p["os"]["family"] = "windows"; p["os"]["build"] = "";
    p["runner_version"] = "control-receiver-0.1";
    p["capabilities"] = capabilities();
    for (const auto field : {"process","file","network","script","registry"}) p["monitoring_coverage"][field] = false;
    if (output_monitor_) {
        check(output_monitor_());
        p["monitoring_coverage"]["file"] = true;
    }
    p["client_nonce"] = random_nonce(); return p;
}
std::string HostSenderSchema::hello_ack(const Envelope& e) const {
    identity(e); check(e.status == "OK" && e.error.isNull() && host_id(e.connection_id));
    const auto& p = e.payload;
    fields(p,{"selected_version","allowed_capabilities","limits","channel_credentials"});
    check(p["selected_version"] == "1.0" && p["allowed_capabilities"].isArray() && p["allowed_capabilities"].size() <= 32);
    const auto advertised = capabilities(); std::set<std::string> seen;
    for (const auto& c : p["allowed_capabilities"]) {
        check(c.isString() && std::find(advertised.begin(),advertised.end(),c) != advertised.end() && seen.insert(c.asString()).second);
    }
    const auto& l = p["limits"];
    fields(l,{"max_message_bytes","max_queue_depth","heartbeat_interval_ms","alive_timeout_ms"});
    check(number(l["max_message_bytes"],1024,65536) && number(l["max_queue_depth"],1,32) &&
          number(l["heartbeat_interval_ms"],1000,60000) && number(l["alive_timeout_ms"],500,30000));
    fields(p["channel_credentials"],{"telemetry","reconnect"});
    credential(p["channel_credentials"]["telemetry"]); credential(p["channel_credentials"]["reconnect"]);
    limit_ = l["max_message_bytes"].asUInt64();
    allowed_ = p["allowed_capabilities"];
    queue_limit_ = l["max_queue_depth"].asUInt64();
    // Credentials are validated but not used before the separate channel contract is agreed.
    return e.connection_id.asString();
}
void HostSenderSchema::validate_request(const Envelope& e) const {
    identity(e); const auto& p = e.payload;
    if (e.type == "HEARTBEAT") { fields(p,{"lease_expires_at"}); date(p["lease_expires_at"]); }
    else if (e.type == "STATE_REQUEST") { fields(p,{"action_id"}); check(p["action_id"].isNull() || text(p["action_id"],1,64)); }
    else if (e.type == "TERMINATE") {
        fields(p,{"reason","grace_ms"}); check(number(p["grace_ms"],0,3000));
        check(one(p["reason"],{"TASK_COMPLETE","USER_STOP","SECURITY_VIOLATION","TIMEOUT","RUNTIME_ERROR","EMERGENCY_KILL"}));
    } else if (e.type == "OBSERVE") {
        check(host_id(e.task_id) && host_id(e.action_id));
        fields(p,{"display_id","capture_format","upload_id"});
        check(p["display_id"] == "primary" && p["capture_format"] == "png"); upload(p["upload_id"]);
    } else if (e.type == "ACTION_REQUEST") { check(host_id(e.task_id) && host_id(e.action_id)); action(p); }
    else if (e.type == "ARTIFACT_REQUEST") {
        // Validate this Host draft to reject it explicitly; do not map candidate_id to #11 events.
        fields(p,{"candidate_id","upload_id","upload_deadline_at","max_bytes"});
        check(text(p["candidate_id"],1,64) && number(p["max_bytes"],1,52428800));
        upload(p["upload_id"]); date(p["upload_deadline_at"]);
    } else check(false);
}
void HostSenderSchema::validate_reply(const Envelope& e) const {
    identity(e); const auto& p = e.payload;
    if (e.type == "ERROR") {
        fields(p,{}); check(e.status == "ERROR");
        fields(e.error,{"code","message","retryable","recommended_next_step"});
        check(e.error["code"] == "UNSUPPORTED_TYPE" && text(e.error["message"],1,1024) &&
              e.error["retryable"].isBool() && (e.error["recommended_next_step"].isNull() || text(e.error["recommended_next_step"],1,64)));
        return;
    }
    if (e.type == "ACK") {
        fields(p,{"queue_position","reject_reason"});
        check(e.error.isNull() && one(e.status,{"ACCEPTED","REJECTED"}));
        if (e.status == "ACCEPTED") check(number(p["queue_position"],0,32) && p["reject_reason"].isNull());
        else check(p["queue_position"].isNull() && text(p["reject_reason"],1,256));
        return;
    }
    if (e.type == "ACTION_RESULT") {
        fields(p,{"execution_time_ms","result"});
        check(e.error.isNull() && one(e.status,{"SUCCESS","PARTIAL_SUCCESS","FAILED","UNKNOWN","BLOCKED"}));
        check(number(p["execution_time_ms"],0,Json::UInt64(-1)));
        const auto& r = p["result"];
        fields(r,{"input_delivered","chars_sent","detail"});
        check(r["input_delivered"].isBool() && number(r["chars_sent"],0,4096) && text(r["detail"],0,1024));
        return;
    }
    check(e.status == "OK" && e.error.isNull());
    if (e.type == "OBSERVE_RESULT") {
        fields(p,{"observation_id","width","height","captured_at","sha256","upload_id"});
        check(host_id(p["observation_id"]) && number(p["width"],1,16384) && number(p["height"],1,16384));
        check(p["width"].asUInt64()*p["height"].asUInt64() <= 16000000);
        date(p["captured_at"]); upload(p["upload_id"]);
        check(text(p["sha256"],64,64) && p["sha256"].asString().find_first_not_of("0123456789abcdef") == std::string::npos);
        return;
    }
    if (e.type == "ALIVE") {
        fields(p,{"runtime_state","worker_alive","queue_depth","uptime_ms"}); runtime(p);
        check(number(p["uptime_ms"],0,Json::UInt64(-1)));
    } else if (e.type == "STATE_RESULT") {
        fields(p,{"runtime_state","worker_alive","queue_depth","action_state"}); runtime(p);
        if (!p["action_state"].isNull()) {
            fields(p["action_state"],{"action_id","status"}); check(text(p["action_state"]["action_id"],1,64));
            check(one(p["action_state"]["status"],{"PENDING","RUNNING","SUCCESS","PARTIAL_SUCCESS","FAILED","UNKNOWN","BLOCKED"}));
        }
    } else if (e.type == "TERMINATE_RESULT") {
        fields(p,{"worker_stopped","pending_actions_dropped"});
        check(p["worker_stopped"].isBool() && number(p["pending_actions_dropped"],0,32));
    } else check(false); // no product GUI success schema until a real worker exists
}
} // namespace scrp
